/* GIF decoder (GIF89a specification). Only the first frame is shown. */
#include "image_internal.h"

#define MAX_PIXELS (4 * 1024 * 1024)
#define MAX_CODES 4096

struct gif {
    const uint8_t *data;
    int length;
    int position;
    int block_left; /* Bytes left in the current data sub-block. */
    int ended;
    uint32_t bits;
    int bit_count;

    uint16_t prefix[MAX_CODES];
    uint8_t suffix[MAX_CODES];
    uint8_t stack[MAX_CODES + 1];
    uint32_t palette[256];
    uint8_t indices[MAX_PIXELS];
    uint32_t row[IMAGE_MAX_SOURCE_SIDE];
    struct scaler scaler;
};

static struct gif g_gif;

/* Reads the next byte of image data, crossing sub-block boundaries. */
static int next_byte(struct gif *g)
{
    while (g->block_left == 0) {
        if (g->ended || g->position >= g->length)
            return -1;
        g->block_left = g->data[g->position++];
        if (g->block_left == 0) {
            g->ended = 1;
            return -1;
        }
    }
    if (g->position >= g->length)
        return -1;
    g->block_left--;
    return g->data[g->position++];
}

static int read_code(struct gif *g, int size)
{
    while (g->bit_count < size) {
        int byte = next_byte(g);
        if (byte < 0)
            return -1;
        g->bits |= (uint32_t)byte << g->bit_count;
        g->bit_count += 8;
    }
    int code = (int)(g->bits & ((1u << size) - 1));
    g->bits >>= size;
    g->bit_count -= size;
    return code;
}

/* Decodes LZW data into indices. Returns the number of pixels produced. */
static int decode_lzw(struct gif *g, int minimum_size, int pixel_count)
{
    int clear = 1 << minimum_size;
    int end = clear + 1;
    int size = minimum_size + 1;
    int next = clear + 2;
    int previous = -1;
    int first = 0;
    int produced = 0;

    for (int i = 0; i < clear; i++) {
        g->prefix[i] = 0;
        g->suffix[i] = (uint8_t)i;
    }

    while (produced < pixel_count) {
        int code = read_code(g, size);
        if (code < 0 || code == end)
            break;
        if (code == clear) {
            size = minimum_size + 1;
            next = clear + 2;
            previous = -1;
            continue;
        }
        if (previous < 0) {
            if (code >= clear)
                return produced;
            g->indices[produced++] = (uint8_t)code;
            previous = code;
            first = code;
            continue;
        }

        int input = code;
        int depth = 0;
        if (code >= next) {
            if (code != next)
                return produced; /* Corrupt stream. */
            g->stack[depth++] = (uint8_t)first;
            code = previous;
        }
        while (code >= clear) {
            if (depth >= MAX_CODES)
                return produced;
            g->stack[depth++] = g->suffix[code];
            code = g->prefix[code];
        }
        first = g->suffix[code];
        g->stack[depth++] = (uint8_t)first;

        while (depth > 0 && produced < pixel_count)
            g->indices[produced++] = g->stack[--depth];

        if (next < MAX_CODES) {
            g->prefix[next] = (uint16_t)previous;
            g->suffix[next] = (uint8_t)first;
            next++;
            if (next == (1 << size) && size < 12)
                size++;
        }
        previous = input;
    }
    return produced;
}

/* Maps a display row to its position in an interlaced frame's data. */
static int interlaced_row(int y, int height)
{
    int pass1 = (height + 7) / 8;
    int pass2 = (height + 3) / 8;
    int pass3 = (height + 1) / 4;
    if (y % 8 == 0)
        return y / 8;
    if (y % 8 == 4)
        return pass1 + y / 8;
    if (y % 4 == 2)
        return pass1 + pass2 + y / 4;
    return pass1 + pass2 + pass3 + y / 2;
}

static int read_palette(struct gif *g, int flags, uint32_t *palette)
{
    int count = 2 << (flags & 7);
    if (g->length - g->position < count * 3)
        return 0;
    for (int i = 0; i < count; i++) {
        const uint8_t *p = g->data + g->position + i * 3;
        palette[i] = ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
    }
    for (int i = count; i < 256; i++)
        palette[i] = 0;
    g->position += count * 3;
    return 1;
}

static int skip_sub_blocks(struct gif *g)
{
    while (g->position < g->length) {
        int size = g->data[g->position++];
        if (size == 0)
            return 1;
        if (size > g->length - g->position)
            return 0;
        g->position += size;
    }
    return 0;
}

int gif_decode(const uint8_t *data, int length, const struct image_request *request,
               struct image_result *result)
{
    struct gif *g = &g_gif;
    uint32_t global_palette[256];
    int has_global = 0;
    int transparent = -1;

    if (length < 13 || data[0] != 'G' || data[1] != 'I' || data[2] != 'F')
        return IMAGE_CORRUPT;
    g->data = data;
    g->length = length;
    g->position = 13;
    int screen_flags = data[10];
    if (screen_flags & 0x80) {
        if (!read_palette(g, screen_flags, global_palette))
            return IMAGE_CORRUPT;
        has_global = 1;
    }

    while (g->position < length) {
        int block = data[g->position++];
        if (block == 0x3b)
            break;
        if (block == 0x21) { /* Extension */
            if (g->position >= length)
                return IMAGE_CORRUPT;
            int label = data[g->position++];
            if (label == 0xf9 && g->position + 5 < length && data[g->position] == 4) {
                if (data[g->position + 1] & 1)
                    transparent = data[g->position + 4];
            }
            if (!skip_sub_blocks(g))
                return IMAGE_CORRUPT;
            continue;
        }
        if (block != 0x2c || length - g->position < 9)
            return IMAGE_CORRUPT;

        int width = data[g->position + 4] | (data[g->position + 5] << 8);
        int height = data[g->position + 6] | (data[g->position + 7] << 8);
        int flags = data[g->position + 8];
        g->position += 9;
        if (width == 0 || height == 0)
            return IMAGE_CORRUPT;
        if ((int64_t)width * height > MAX_PIXELS)
            return IMAGE_TOO_LARGE;

        if (flags & 0x80) {
            if (!read_palette(g, flags, g->palette))
                return IMAGE_CORRUPT;
        } else if (has_global) {
            for (int i = 0; i < 256; i++)
                g->palette[i] = global_palette[i];
        } else {
            for (int i = 0; i < 256; i++)
                g->palette[i] = ((uint32_t)i << 16) | ((uint32_t)i << 8) | (uint32_t)i;
        }

        if (g->position >= length)
            return IMAGE_CORRUPT;
        int minimum_size = data[g->position++];
        if (minimum_size < 2 || minimum_size > 8)
            return IMAGE_CORRUPT;

        int status = scaler_begin(&g->scaler, width, height, request, result);
        if (status != IMAGE_OK)
            return status;

        g->block_left = 0;
        g->ended = 0;
        g->bits = 0;
        g->bit_count = 0;
        int pixels = width * height;
        int produced = decode_lzw(g, minimum_size, pixels);
        /* Missing pixels in a short stream show as the background, as browsers do. */
        for (int i = produced; i < pixels; i++)
            g->indices[i] = transparent >= 0 ? (uint8_t)transparent : 0;
        if (produced == 0)
            return IMAGE_CORRUPT;

        for (int y = 0; y < height; y++) {
            int source = (flags & 0x40) ? interlaced_row(y, height) : y;
            const uint8_t *line = g->indices + (int64_t)source * width;
            for (int x = 0; x < width; x++) {
                int index = line[x];
                g->row[x] = index == transparent ? request->background : g->palette[index];
            }
            scaler_row(&g->scaler, g->row);
        }
        return scaler_end(&g->scaler);
    }
    return IMAGE_CORRUPT;
}
