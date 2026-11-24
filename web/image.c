#include "image_internal.h"

const char *image_status_text(int status)
{
    switch (status) {
    case IMAGE_OK:
        return "ok";
    case IMAGE_UNSUPPORTED:
        return "unsupported format";
    case IMAGE_TOO_LARGE:
        return "too large";
    default:
        return "damaged";
    }
}

/* value * numerator / denominator, rounded to the nearest whole number. */
static int64_t scale_round(int64_t value, int64_t numerator, int64_t denominator)
{
    return (value * numerator + denominator / 2) / denominator;
}

int image_fit(int source_width, int source_height, const struct image_request *request,
              int *width, int *height)
{
    if (source_width <= 0 || source_height <= 0)
        return 0;

    /* Start from the HTML size if given, keeping the aspect ratio when only one is set. */
    int64_t w = source_width;
    int64_t h = source_height;
    if (request->want_width > 0 && request->want_height > 0) {
        w = request->want_width;
        h = request->want_height;
    } else if (request->want_width > 0) {
        w = request->want_width;
        h = scale_round(source_height, w, source_width);
    } else if (request->want_height > 0) {
        h = request->want_height;
        w = scale_round(source_width, h, source_height);
    }

    /* Never enlarge, because the decoders only shrink. */
    if (w > source_width) {
        h = scale_round(h, source_width, w);
        w = source_width;
    }
    if (h > source_height) {
        w = scale_round(w, source_height, h);
        h = source_height;
    }

    int64_t max_w = request->max_width < IMAGE_MAX_OUTPUT_WIDTH ? request->max_width
                                                                : IMAGE_MAX_OUTPUT_WIDTH;
    if (w > max_w) {
        h = scale_round(h, max_w, w);
        w = max_w;
    }
    if (h > request->max_height) {
        w = scale_round(w, request->max_height, h);
        h = request->max_height;
    }
    while (w * h > request->capacity && w > 1 && h > 1) {
        w = w * 3 / 4;
        h = h * 3 / 4;
    }
    if (w < 1)
        w = 1;
    if (h < 1)
        h = 1;
    if (w * h > request->capacity)
        return 0;
    *width = (int)w;
    *height = (int)h;
    return 1;
}

int scaler_begin(struct scaler *s, int source_width, int source_height,
                 const struct image_request *request, struct image_result *result)
{
    if (source_width <= 0 || source_height <= 0)
        return IMAGE_CORRUPT;
    if (source_width > IMAGE_MAX_SOURCE_SIDE || source_height > IMAGE_MAX_SOURCE_SIDE)
        return IMAGE_TOO_LARGE;

    int width, height;
    if (!image_fit(source_width, source_height, request, &width, &height))
        return IMAGE_TOO_LARGE;

    s->source_width = source_width;
    s->source_height = source_height;
    s->width = width;
    s->height = height;
    s->out = request->pixels;
    s->source_row = 0;
    s->row = 0;
    for (int x = 0; x < width; x++) {
        s->red[x] = 0;
        s->green[x] = 0;
        s->blue[x] = 0;
        s->count[x] = 0;
    }
    result->width = width;
    result->height = height;
    return IMAGE_OK;
}

static void flush_row(struct scaler *s)
{
    uint32_t *line = s->out + (int64_t)s->row * s->width;
    for (int x = 0; x < s->width; x++) {
        uint32_t n = s->count[x] ? s->count[x] : 1;
        line[x] = ((s->red[x] / n) << 16) | ((s->green[x] / n) << 8) | (s->blue[x] / n);
        s->red[x] = 0;
        s->green[x] = 0;
        s->blue[x] = 0;
        s->count[x] = 0;
    }
}

void scaler_row(struct scaler *s, const uint32_t *row)
{
    if (s->source_row >= s->source_height)
        return;

    int target = (int)((int64_t)s->source_row * s->height / s->source_height);
    if (target != s->row) {
        flush_row(s);
        s->row = target;
    }

    /* Each output column sums the source pixels that map onto it. */
    int x = 0;
    for (int column = 0; column < s->width; column++) {
        int end = (int)(((int64_t)(column + 1) * s->source_width + s->width - 1) / s->width);
        if (end > s->source_width)
            end = s->source_width;
        for (; x < end; x++) {
            uint32_t pixel = row[x];
            s->red[column] += (pixel >> 16) & 0xff;
            s->green[column] += (pixel >> 8) & 0xff;
            s->blue[column] += pixel & 0xff;
            s->count[column]++;
        }
    }
    s->source_row++;
}

int scaler_end(struct scaler *s)
{
    if (s->source_row != s->source_height)
        return IMAGE_CORRUPT;
    flush_row(s);
    return IMAGE_OK;
}

int image_decode(const uint8_t *data, int length, const struct image_request *request,
                 struct image_result *result)
{
    if (length >= 8 && data[0] == 0x89 && data[1] == 'P' && data[2] == 'N' && data[3] == 'G')
        return png_decode(data, length, request, result);
    if (length >= 3 && data[0] == 0xff && data[1] == 0xd8 && data[2] == 0xff)
        return jpeg_decode(data, length, request, result);
    if (length >= 6 && data[0] == 'G' && data[1] == 'I' && data[2] == 'F' && data[3] == '8')
        return gif_decode(data, length, request, result);
    return IMAGE_UNSUPPORTED;
}
