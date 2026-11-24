/*
 * Image decoding for the browser: PNG, baseline JPEG, and GIF (first frame).
 * Images are scaled down while decoding, straight into the caller's buffer, so
 * memory use is bounded by the displayed size rather than the file's size.
 * All input is untrusted and every read is bounds-checked.
 */
#ifndef CATO_WEB_IMAGE_H
#define CATO_WEB_IMAGE_H

#include <stdint.h>

#define IMAGE_MAX_SOURCE_SIDE 16384
#define IMAGE_MAX_OUTPUT_WIDTH 2048

enum image_status {
    IMAGE_OK = 0,
    IMAGE_UNSUPPORTED = -1, /* For example progressive JPEG, WebP, or SVG. */
    IMAGE_CORRUPT = -2,
    IMAGE_TOO_LARGE = -3,
};

struct image_request {
    int max_width;  /* Display bounds in pixels. */
    int max_height;
    int want_width; /* Size from the HTML width and height attributes, or 0. */
    int want_height;
    uint32_t background; /* Transparent pixels are blended onto this 0x00RRGGBB color. */
    uint32_t *pixels;    /* Output, 0x00RRGGBB, row-major. */
    int capacity;        /* Pixels available in the output. */
};

struct image_result {
    int width;
    int height;
};

int image_decode(const uint8_t *data, int length, const struct image_request *request,
                 struct image_result *result);
const char *image_status_text(int status);

/* Chooses the displayed size, never enlarging the source. Returns 0 if it cannot fit. */
int image_fit(int source_width, int source_height, const struct image_request *request,
              int *width, int *height);

#endif
