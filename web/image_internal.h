/* Shared pieces of the image decoders. */
#ifndef CATO_WEB_IMAGE_INTERNAL_H
#define CATO_WEB_IMAGE_INTERNAL_H

#include "image.h"

/* Box-filter downscaler fed one source row at a time. */
struct scaler {
    int source_width;
    int source_height;
    int width;
    int height;
    uint32_t *out;
    int source_row;
    int row;
    uint32_t red[IMAGE_MAX_OUTPUT_WIDTH];
    uint32_t green[IMAGE_MAX_OUTPUT_WIDTH];
    uint32_t blue[IMAGE_MAX_OUTPUT_WIDTH];
    uint32_t count[IMAGE_MAX_OUTPUT_WIDTH];
};

/* Validates the source size and prepares the scaler. Returns an image_status. */
int scaler_begin(struct scaler *s, int source_width, int source_height,
                 const struct image_request *request, struct image_result *result);
/* Adds one row of source_width 0x00RRGGBB pixels. */
void scaler_row(struct scaler *s, const uint32_t *row);
/* Returns IMAGE_OK only if every source row arrived. */
int scaler_end(struct scaler *s);

static inline uint32_t blend(uint32_t red, uint32_t green, uint32_t blue, uint32_t alpha,
                             uint32_t background)
{
    uint32_t inverse = 255 - alpha;
    uint32_t r = (red * alpha + ((background >> 16) & 0xff) * inverse + 127) / 255;
    uint32_t g = (green * alpha + ((background >> 8) & 0xff) * inverse + 127) / 255;
    uint32_t b = (blue * alpha + (background & 0xff) * inverse + 127) / 255;
    return (r << 16) | (g << 8) | b;
}

/* Streaming DEFLATE (RFC 1951). The source returns a byte, or -1 at the end. */
struct inflate_source {
    int (*read)(void *context);
    void *context;
};
/* The sink returns 0 to stop early, for example once an image is complete. */
typedef int (*inflate_sink)(void *context, const uint8_t *data, int length);
/* Returns 0 at the end of the stream, 1 if the sink stopped it, or -1 if corrupt. */
int inflate_stream(struct inflate_source source, inflate_sink sink, void *context);

int png_decode(const uint8_t *data, int length, const struct image_request *request,
               struct image_result *result);
int jpeg_decode(const uint8_t *data, int length, const struct image_request *request,
                struct image_result *result);
int gif_decode(const uint8_t *data, int length, const struct image_request *request,
               struct image_result *result);

#endif
