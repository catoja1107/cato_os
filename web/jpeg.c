/*
 * Baseline JPEG decoder (ITU-T T.81): sequential Huffman coding, 8-bit samples,
 * grayscale or YCbCr, with sampling factors up to 2. Progressive JPEG is not
 * supported, and the caller shows a placeholder instead.
 */
#include "image_internal.h"

#define MAX_COMPONENTS 3
#define MAX_WIDTH 8192
#define BUFFER_STRIDE (MAX_WIDTH + 32)

struct huffman_table {
    int present;
    int max_code[18];
    int value_offset[17];
    int min_code[17];
    uint8_t values[256];
};

struct component {
    int id;
    int h;
    int v;
    int quant;
    int dc_table;
    int ac_table;
    int predictor;
    uint8_t *samples; /* One MCU row of this component. */
};

struct jpeg {
    int width;
    int height;
    int component_count;
    int max_h;
    int max_v;
    int restart_interval;
    struct component components[MAX_COMPONENTS];
    uint16_t quant[4][64];
    int quant_present[4];
    struct huffman_table dc[4];
    struct huffman_table ac[4];

    const uint8_t *data;
    int length;
    int position;
    uint32_t bits;
    int bit_count;
    int hit_marker;
    int error;

    uint32_t background;
    struct scaler scaler;
    uint8_t planes[MAX_COMPONENTS][BUFFER_STRIDE * 16];
    uint32_t row[MAX_WIDTH];
};

static struct jpeg g_jpeg;

static const uint8_t ZIGZAG[64] = {
    0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5,
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63,
};

/* IDCT basis: C(u) * cos((2x + 1) * u * pi / 16), scaled by 4096. */
static int32_t g_basis[8][8];
static int g_basis_ready;

static void build_basis(void)
{
    /* cos(k * pi / 16) * 4096 for k = 0..8. */
    static const int32_t COSINE[9] = { 4096, 4017, 3784, 3406, 2896, 2276, 1567, 799, 0 };
    for (int x = 0; x < 8; x++) {
        for (int u = 0; u < 8; u++) {
            int m = ((2 * x + 1) * u) % 32;
            int sign = 1;
            if (m > 16)
                m = 32 - m;
            if (m > 8) {
                m = 16 - m;
                sign = -1;
            }
            int32_t value = sign * COSINE[m];
            if (u == 0)
                value = value * 2896 / 4096; /* C(0) = 1 / sqrt(2). */
            g_basis[x][u] = value;
        }
    }
    g_basis_ready = 1;
}

static int read16(const uint8_t *p)
{
    return (p[0] << 8) | p[1];
}

static int build_table(struct huffman_table *table, const uint8_t *counts, const uint8_t *symbols,
                       int total)
{
    int code = 0, k = 0;
    for (int i = 0; i < total; i++)
        table->values[i] = symbols[i];
    for (int length = 1; length <= 16; length++) {
        table->value_offset[length] = k;
        table->min_code[length] = code;
        code += counts[length - 1];
        k += counts[length - 1];
        table->max_code[length] = counts[length - 1] ? code - 1 : -1;
        if (code > (1 << length))
            return 0;
        code <<= 1;
    }
    table->max_code[17] = 0x7fffffff;
    table->present = 1;
    return 1;
}

/* Reads entropy-coded bits, removing stuffed zero bytes. A marker stops the data. */
static int read_bit(struct jpeg *j)
{
    if (j->bit_count == 0) {
        int byte = 0;
        if (!j->hit_marker && j->position < j->length) {
            byte = j->data[j->position];
            if (byte == 0xff) {
                int next = j->position + 1 < j->length ? j->data[j->position + 1] : 0xd9;
                if (next == 0) {
                    j->position += 2;
                } else {
                    j->hit_marker = 1;
                    byte = 0;
                }
            } else {
                j->position++;
            }
        }
        j->bits = (uint32_t)byte;
        j->bit_count = 8;
    }
    j->bit_count--;
    return (int)((j->bits >> j->bit_count) & 1);
}

static int read_bits(struct jpeg *j, int count)
{
    int value = 0;
    for (int i = 0; i < count; i++)
        value = (value << 1) | read_bit(j);
    return value;
}

static int decode_symbol(struct jpeg *j, const struct huffman_table *table)
{
    int code = read_bit(j);
    for (int length = 1; length <= 16; length++) {
        if (code <= table->max_code[length])
            return table->values[table->value_offset[length] + code - table->min_code[length]];
        code = (code << 1) | read_bit(j);
    }
    j->error = 1;
    return 0;
}

static int extend(int value, int size)
{
    return size == 0 ? 0 : value < (1 << (size - 1)) ? value - (1 << size) + 1 : value;
}

static void idct_block(const int32_t *coefficients, uint8_t *out, int stride)
{
    int64_t temp[64];
    for (int v = 0; v < 8; v++) {
        for (int x = 0; x < 8; x++) {
            int64_t sum = 0;
            for (int u = 0; u < 8; u++)
                sum += (int64_t)g_basis[x][u] * coefficients[v * 8 + u];
            temp[v * 8 + x] = sum;
        }
    }
    for (int y = 0; y < 8; y++) {
        for (int x = 0; x < 8; x++) {
            int64_t sum = 0;
            for (int v = 0; v < 8; v++)
                sum += (int64_t)g_basis[y][v] * temp[v * 8 + x];
            /* Two 1/2 factors from the 2-D IDCT and two 4096 scales from the basis. */
            int64_t value = (sum + ((int64_t)1 << 25)) >> 26;
            value += 128;
            out[y * stride + x] = (uint8_t)(value < 0 ? 0 : value > 255 ? 255 : value);
        }
    }
}

static void decode_block(struct jpeg *j, struct component *c, uint8_t *out, int stride)
{
    int32_t coefficients[64];
    const uint16_t *quant = j->quant[c->quant];
    for (int i = 0; i < 64; i++)
        coefficients[i] = 0;

    int size = decode_symbol(j, &j->dc[c->dc_table]);
    if (size > 11) {
        j->error = 1;
        return;
    }
    c->predictor += extend(read_bits(j, size), size);
    coefficients[0] = c->predictor * quant[0];

    for (int k = 1; k < 64;) {
        int symbol = decode_symbol(j, &j->ac[c->ac_table]);
        int run = symbol >> 4;
        size = symbol & 15;
        if (size == 0) {
            if (run != 15)
                break; /* End of block. */
            k += 16;
            continue;
        }
        k += run;
        if (k > 63 || size > 10) {
            j->error = 1;
            return;
        }
        coefficients[ZIGZAG[k]] = extend(read_bits(j, size), size) * quant[ZIGZAG[k]];
        k++;
    }
    idct_block(coefficients, out, stride);
}

static int clamp255(int value)
{
    return value < 0 ? 0 : value > 255 ? 255 : value;
}

static void emit_rows(struct jpeg *j, int mcu_row)
{
    int rows = j->max_v * 8;
    for (int y = 0; y < rows; y++) {
        int image_y = mcu_row * rows + y;
        if (image_y >= j->height)
            return;
        for (int x = 0; x < j->width; x++) {
            int value[MAX_COMPONENTS];
            for (int i = 0; i < j->component_count; i++) {
                const struct component *c = &j->components[i];
                int sx = x * c->h / j->max_h;
                int sy = y * c->v / j->max_v;
                value[i] = c->samples[sy * BUFFER_STRIDE + sx];
            }
            if (j->component_count == 1) {
                uint32_t gray = (uint32_t)value[0];
                j->row[x] = (gray << 16) | (gray << 8) | gray;
            } else {
                int luma = value[0] << 16;
                int cb = value[1] - 128;
                int cr = value[2] - 128;
                int r = clamp255((luma + 91881 * cr + 32768) >> 16);
                int g = clamp255((luma - 22554 * cb - 46802 * cr + 32768) >> 16);
                int b = clamp255((luma + 116130 * cb + 32768) >> 16);
                j->row[x] = ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
            }
        }
        scaler_row(&j->scaler, j->row);
    }
}

/* Moves past an RSTn marker and resets the decoder state. */
static void restart(struct jpeg *j)
{
    j->bit_count = 0;
    j->hit_marker = 0;
    while (j->position + 1 < j->length) {
        if (j->data[j->position] == 0xff && j->data[j->position + 1] >= 0xd0 &&
            j->data[j->position + 1] <= 0xd7) {
            j->position += 2;
            break;
        }
        j->position++;
    }
    for (int i = 0; i < j->component_count; i++)
        j->components[i].predictor = 0;
}

static int decode_scan(struct jpeg *j)
{
    int mcu_width = j->max_h * 8;
    int mcu_height = j->max_v * 8;
    int mcus_x = (j->width + mcu_width - 1) / mcu_width;
    int mcus_y = (j->height + mcu_height - 1) / mcu_height;
    int until_restart = j->restart_interval;

    for (int i = 0; i < j->component_count; i++) {
        j->components[i].samples = j->planes[i];
        j->components[i].predictor = 0;
    }

    for (int my = 0; my < mcus_y; my++) {
        for (int mx = 0; mx < mcus_x; mx++) {
            if (j->restart_interval) {
                if (until_restart == 0) {
                    restart(j);
                    until_restart = j->restart_interval;
                }
                until_restart--;
            }
            for (int i = 0; i < j->component_count; i++) {
                struct component *c = &j->components[i];
                for (int by = 0; by < c->v; by++) {
                    for (int bx = 0; bx < c->h; bx++) {
                        int x = (mx * c->h + bx) * 8;
                        decode_block(j, c, c->samples + by * 8 * BUFFER_STRIDE + x, BUFFER_STRIDE);
                        if (j->error)
                            return IMAGE_CORRUPT;
                    }
                }
            }
        }
        emit_rows(j, my);
    }
    return scaler_end(&j->scaler);
}

int jpeg_decode(const uint8_t *data, int length, const struct image_request *request,
                struct image_result *result)
{
    struct jpeg *j = &g_jpeg;
    int have_frame = 0;

    if (!g_basis_ready)
        build_basis();
    j->data = data;
    j->length = length;
    j->restart_interval = 0;
    j->error = 0;
    j->background = request->background;
    for (int i = 0; i < 4; i++) {
        j->dc[i].present = 0;
        j->ac[i].present = 0;
        j->quant_present[i] = 0;
    }

    int p = 2;
    for (;;) {
        while (p < length && data[p] != 0xff)
            p++;
        while (p < length && data[p] == 0xff)
            p++;
        if (p >= length)
            return IMAGE_CORRUPT;
        int marker = data[p++];

        if (marker == 0xd8 || (marker >= 0xd0 && marker <= 0xd7) || marker == 0x01)
            continue;
        if (marker == 0xd9)
            return IMAGE_CORRUPT; /* End of image before any scan. */
        if (length - p < 2)
            return IMAGE_CORRUPT;
        int size = read16(data + p);
        if (size < 2 || size > length - p)
            return IMAGE_CORRUPT;
        const uint8_t *body = data + p + 2;
        int body_length = size - 2;

        if (marker == 0xdb) { /* Quantization tables */
            for (int q = 0; q < body_length;) {
                int precision = body[q] >> 4;
                int id = body[q] & 15;
                int needed = 1 + 64 * (precision ? 2 : 1);
                if (id > 3 || precision > 1 || q + needed > body_length)
                    return IMAGE_CORRUPT;
                for (int k = 0; k < 64; k++)
                    j->quant[id][ZIGZAG[k]] = precision ? (uint16_t)read16(body + q + 1 + k * 2)
                                                        : body[q + 1 + k];
                j->quant_present[id] = 1;
                q += needed;
            }
        } else if (marker == 0xc4) { /* Huffman tables */
            for (int q = 0; q < body_length;) {
                if (q + 17 > body_length)
                    return IMAGE_CORRUPT;
                int table_class = body[q] >> 4;
                int id = body[q] & 15;
                int total = 0;
                for (int k = 0; k < 16; k++)
                    total += body[q + 1 + k];
                if (table_class > 1 || id > 3 || total > 256 || q + 17 + total > body_length)
                    return IMAGE_CORRUPT;
                struct huffman_table *table = table_class ? &j->ac[id] : &j->dc[id];
                if (!build_table(table, body + q + 1, body + q + 17, total))
                    return IMAGE_CORRUPT;
                q += 17 + total;
            }
        } else if (marker == 0xc0 || marker == 0xc1) { /* Baseline or extended sequential */
            if (body_length < 6 || body[0] != 8)
                return IMAGE_UNSUPPORTED;
            j->height = read16(body + 1);
            j->width = read16(body + 3);
            j->component_count = body[5];
            if (j->width == 0 || j->height == 0)
                return IMAGE_UNSUPPORTED; /* Height defined later by DNL is not supported. */
            if (j->width > MAX_WIDTH)
                return IMAGE_TOO_LARGE;
            if ((j->component_count != 1 && j->component_count != 3) ||
                body_length < 6 + j->component_count * 3)
                return IMAGE_UNSUPPORTED;
            j->max_h = 1;
            j->max_v = 1;
            for (int i = 0; i < j->component_count; i++) {
                struct component *c = &j->components[i];
                c->id = body[6 + i * 3];
                c->h = body[7 + i * 3] >> 4;
                c->v = body[7 + i * 3] & 15;
                c->quant = body[8 + i * 3];
                if (c->h < 1 || c->h > 2 || c->v < 1 || c->v > 2 || c->quant > 3)
                    return IMAGE_UNSUPPORTED;
                j->max_h = c->h > j->max_h ? c->h : j->max_h;
                j->max_v = c->v > j->max_v ? c->v : j->max_v;
            }
            have_frame = 1;
        } else if ((marker >= 0xc2 && marker <= 0xcf) && marker != 0xc4 && marker != 0xc8 &&
                   marker != 0xcc) {
            return IMAGE_UNSUPPORTED; /* Progressive, lossless, or arithmetic coding. */
        } else if (marker == 0xdd) {
            if (body_length < 2)
                return IMAGE_CORRUPT;
            j->restart_interval = read16(body);
        } else if (marker == 0xda) { /* Start of scan */
            if (!have_frame || body_length < 1)
                return IMAGE_CORRUPT;
            int count = body[0];
            if (count != j->component_count || body_length < 1 + count * 2 + 3)
                return IMAGE_UNSUPPORTED; /* Multiple scans are not supported. */
            for (int i = 0; i < count; i++) {
                int id = body[1 + i * 2];
                int tables = body[2 + i * 2];
                struct component *c = 0;
                for (int k = 0; k < j->component_count; k++) {
                    if (j->components[k].id == id)
                        c = &j->components[k];
                }
                if (!c || (tables >> 4) > 3 || (tables & 15) > 3)
                    return IMAGE_CORRUPT;
                c->dc_table = tables >> 4;
                c->ac_table = tables & 15;
                if (!j->dc[c->dc_table].present || !j->ac[c->ac_table].present ||
                    !j->quant_present[c->quant])
                    return IMAGE_CORRUPT;
            }

            int status = scaler_begin(&j->scaler, j->width, j->height, request, result);
            if (status != IMAGE_OK)
                return status;
            j->position = p + size;
            j->bits = 0;
            j->bit_count = 0;
            j->hit_marker = 0;
            return decode_scan(j);
        }
        p += size;
    }
}
