/* PNG decoder (ISO/IEC 15948). Non-interlaced images in every standard color format. */
#include "image_internal.h"

#define MAX_SEGMENTS 8192
#define MAX_STRIDE (IMAGE_MAX_SOURCE_SIDE * 8 + 1)

enum { COLOR_GRAY = 0, COLOR_RGB = 2, COLOR_PALETTE = 3, COLOR_GRAY_ALPHA = 4, COLOR_RGBA = 6 };

struct segment {
    const uint8_t *data;
    int length;
};

struct png {
    int width;
    int height;
    int depth;
    int color;
    int channels;
    int bytes_per_pixel; /* For filtering, at least 1. */
    int stride;          /* Bytes per scanline, without the filter byte. */
    uint32_t palette[256];
    uint8_t palette_alpha[256];
    int palette_size;
    int has_key;
    uint16_t key[3]; /* tRNS color key for gray or RGB images. */
    uint32_t background;

    struct segment segments[MAX_SEGMENTS];
    int segment_count;
    int segment;
    int offset;

    int filled; /* Bytes of the current row received, including the filter byte. */
    int rows_done;
    int error;
    struct scaler scaler;
    uint8_t current[MAX_STRIDE];
    uint8_t previous[MAX_STRIDE];
    uint32_t row[IMAGE_MAX_SOURCE_SIDE];
};

static struct png g_png;

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static int read_compressed(void *context)
{
    struct png *png = context;
    while (png->segment < png->segment_count) {
        const struct segment *s = &png->segments[png->segment];
        if (png->offset < s->length)
            return s->data[png->offset++];
        png->segment++;
        png->offset = 0;
    }
    return -1;
}

static int paeth(int a, int b, int c)
{
    int p = a + b - c;
    int pa = p > a ? p - a : a - p;
    int pb = p > b ? p - b : b - p;
    int pc = p > c ? p - c : c - p;
    if (pa <= pb && pa <= pc)
        return a;
    return pb <= pc ? b : c;
}

static int unfilter(struct png *png)
{
    uint8_t *line = png->current + 1;
    const uint8_t *above = png->previous + 1;
    int bpp = png->bytes_per_pixel;
    int type = png->current[0];

    for (int i = 0; i < png->stride; i++) {
        int left = i >= bpp ? line[i - bpp] : 0;
        int up = above[i];
        int corner = i >= bpp ? above[i - bpp] : 0;
        switch (type) {
        case 0:
            break;
        case 1:
            line[i] = (uint8_t)(line[i] + left);
            break;
        case 2:
            line[i] = (uint8_t)(line[i] + up);
            break;
        case 3:
            line[i] = (uint8_t)(line[i] + ((left + up) >> 1));
            break;
        case 4:
            line[i] = (uint8_t)(line[i] + paeth(left, up, corner));
            break;
        default:
            return 0;
        }
    }
    return 1;
}

static int sample(const uint8_t *line, int index, int depth)
{
    if (depth == 8)
        return line[index];
    if (depth == 16)
        return (line[index * 2] << 8) | line[index * 2 + 1];
    int per_byte = 8 / depth;
    int shift = 8 - depth * (index % per_byte + 1);
    return (line[index / per_byte] >> shift) & ((1 << depth) - 1);
}

/* Scales a sample to 8 bits. */
static uint32_t to8(int value, int depth)
{
    switch (depth) {
    case 1:
        return value ? 255 : 0;
    case 2:
        return (uint32_t)value * 85;
    case 4:
        return (uint32_t)value * 17;
    case 16:
        return (uint32_t)value >> 8;
    default:
        return (uint32_t)value;
    }
}

static void convert_row(struct png *png)
{
    const uint8_t *line = png->current + 1;
    int depth = png->depth;

    for (int x = 0; x < png->width; x++) {
        uint32_t r, g, b, a = 255;
        switch (png->color) {
        case COLOR_GRAY: {
            int v = sample(line, x, depth);
            r = g = b = to8(v, depth);
            if (png->has_key && v == png->key[0])
                a = 0;
            break;
        }
        case COLOR_RGB: {
            int vr = sample(line, x * 3, depth);
            int vg = sample(line, x * 3 + 1, depth);
            int vb = sample(line, x * 3 + 2, depth);
            r = to8(vr, depth);
            g = to8(vg, depth);
            b = to8(vb, depth);
            if (png->has_key && vr == png->key[0] && vg == png->key[1] && vb == png->key[2])
                a = 0;
            break;
        }
        case COLOR_PALETTE: {
            int index = sample(line, x, depth);
            if (index >= png->palette_size) {
                png->error = 1;
                return;
            }
            uint32_t color = png->palette[index];
            r = (color >> 16) & 0xff;
            g = (color >> 8) & 0xff;
            b = color & 0xff;
            a = png->palette_alpha[index];
            break;
        }
        case COLOR_GRAY_ALPHA:
            r = g = b = to8(sample(line, x * 2, depth), depth);
            a = to8(sample(line, x * 2 + 1, depth), depth);
            break;
        default:
            r = to8(sample(line, x * 4, depth), depth);
            g = to8(sample(line, x * 4 + 1, depth), depth);
            b = to8(sample(line, x * 4 + 2, depth), depth);
            a = to8(sample(line, x * 4 + 3, depth), depth);
            break;
        }
        png->row[x] = a == 255 ? (r << 16) | (g << 8) | b : blend(r, g, b, a, png->background);
    }
}

static int take_bytes(void *context, const uint8_t *data, int length)
{
    struct png *png = context;
    for (int i = 0; i < length; i++) {
        if (png->rows_done >= png->height)
            return 0; /* Extra data after the last row is ignored. */
        png->current[png->filled++] = data[i];
        if (png->filled < png->stride + 1)
            continue;

        if (!unfilter(png)) {
            png->error = 1;
            return 0;
        }
        convert_row(png);
        if (png->error)
            return 0;
        scaler_row(&png->scaler, png->row);
        for (int k = 0; k <= png->stride; k++)
            png->previous[k] = png->current[k];
        png->filled = 0;
        png->rows_done++;
    }
    return png->rows_done < png->height;
}

static int valid_format(int color, int depth)
{
    switch (color) {
    case COLOR_GRAY:
        return depth == 1 || depth == 2 || depth == 4 || depth == 8 || depth == 16;
    case COLOR_PALETTE:
        return depth == 1 || depth == 2 || depth == 4 || depth == 8;
    case COLOR_RGB:
    case COLOR_GRAY_ALPHA:
    case COLOR_RGBA:
        return depth == 8 || depth == 16;
    default:
        return 0;
    }
}

int png_decode(const uint8_t *data, int length, const struct image_request *request,
               struct image_result *result)
{
    static const uint8_t SIGNATURE[8] = { 0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a };
    struct png *png = &g_png;
    int have_header = 0, interlaced = 0;

    for (int i = 0; i < 8; i++) {
        if (length < 8 || data[i] != SIGNATURE[i])
            return IMAGE_CORRUPT;
    }
    png->segment_count = 0;
    png->palette_size = 0;
    png->has_key = 0;
    for (int i = 0; i < 256; i++)
        png->palette_alpha[i] = 255;

    int position = 8;
    for (;;) {
        if (length - position < 12)
            return IMAGE_CORRUPT;
        uint32_t size = be32(data + position);
        const uint8_t *type = data + position + 4;
        const uint8_t *body = data + position + 8;
        if (size > (uint32_t)(length - position - 12))
            return IMAGE_CORRUPT;

        if (type[0] == 'I' && type[1] == 'H' && type[2] == 'D' && type[3] == 'R') {
            if (size != 13 || have_header)
                return IMAGE_CORRUPT;
            png->width = (int)be32(body) > 0 ? (int)be32(body) : -1;
            png->height = (int)be32(body + 4) > 0 ? (int)be32(body + 4) : -1;
            png->depth = body[8];
            png->color = body[9];
            interlaced = body[12];
            if (be32(body) > IMAGE_MAX_SOURCE_SIDE || be32(body + 4) > IMAGE_MAX_SOURCE_SIDE)
                return IMAGE_TOO_LARGE;
            if (png->width <= 0 || png->height <= 0 || !valid_format(png->color, png->depth) ||
                body[10] != 0 || body[11] != 0 || interlaced > 1)
                return IMAGE_CORRUPT;
            have_header = 1;
        } else if (!have_header) {
            return IMAGE_CORRUPT;
        } else if (type[0] == 'P' && type[1] == 'L' && type[2] == 'T' && type[3] == 'E') {
            if (size % 3 != 0 || size > 768)
                return IMAGE_CORRUPT;
            png->palette_size = (int)size / 3;
            for (int i = 0; i < png->palette_size; i++)
                png->palette[i] = ((uint32_t)body[i * 3] << 16) | ((uint32_t)body[i * 3 + 1] << 8) |
                                  body[i * 3 + 2];
        } else if (type[0] == 't' && type[1] == 'R' && type[2] == 'N' && type[3] == 'S') {
            if (png->color == COLOR_PALETTE && size <= 256) {
                for (uint32_t i = 0; i < size; i++)
                    png->palette_alpha[i] = body[i];
            } else if (png->color == COLOR_GRAY && size == 2) {
                png->has_key = 1;
                png->key[0] = (uint16_t)((body[0] << 8) | body[1]);
            } else if (png->color == COLOR_RGB && size == 6) {
                png->has_key = 1;
                for (int i = 0; i < 3; i++)
                    png->key[i] = (uint16_t)((body[i * 2] << 8) | body[i * 2 + 1]);
            }
        } else if (type[0] == 'I' && type[1] == 'D' && type[2] == 'A' && type[3] == 'T') {
            if (png->segment_count == MAX_SEGMENTS)
                return IMAGE_TOO_LARGE;
            png->segments[png->segment_count].data = body;
            png->segments[png->segment_count].length = (int)size;
            png->segment_count++;
        } else if (type[0] == 'I' && type[1] == 'E' && type[2] == 'N' && type[3] == 'D') {
            break;
        }
        position += (int)size + 12;
    }

    if (!have_header || png->segment_count == 0)
        return IMAGE_CORRUPT;
    if (png->color == COLOR_PALETTE && png->palette_size == 0)
        return IMAGE_CORRUPT;
    if (interlaced)
        return IMAGE_UNSUPPORTED; /* Deliberate shortcut: Adam7 interlacing is not decoded. */

    static const int CHANNELS[7] = { 1, 0, 3, 1, 2, 0, 4 };
    png->channels = CHANNELS[png->color];
    int bits_per_pixel = png->channels * png->depth;
    png->bytes_per_pixel = bits_per_pixel >= 8 ? bits_per_pixel / 8 : 1;
    png->stride = (int)(((int64_t)png->width * bits_per_pixel + 7) / 8);
    png->background = request->background;

    int status = scaler_begin(&png->scaler, png->width, png->height, request, result);
    if (status != IMAGE_OK)
        return status;

    /* The zlib header (RFC 1950) comes before the DEFLATE data. */
    png->segment = 0;
    png->offset = 0;
    int cmf = read_compressed(png);
    int flags = read_compressed(png);
    if (cmf < 0 || flags < 0 || (cmf & 15) != 8 || (cmf >> 4) > 7 || ((cmf << 8) | flags) % 31 != 0 ||
        (flags & 0x20))
        return IMAGE_CORRUPT;

    for (int i = 0; i <= png->stride; i++)
        png->previous[i] = 0;
    png->filled = 0;
    png->rows_done = 0;
    png->error = 0;

    struct inflate_source source = { read_compressed, png };
    int inflated = inflate_stream(source, take_bytes, png);
    if (png->error || (inflated < 0 && png->rows_done < png->height))
        return IMAGE_CORRUPT;
    return scaler_end(&png->scaler);
}
