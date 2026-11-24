#include "splash.h"

#include "gop.h"

/* Generated from assets/splash.png by scripts/mksplash.py. */
extern const uint32_t splash_width;
extern const uint32_t splash_height;
extern const uint8_t splash_pixels[];

void splash_render(uint32_t *pixels, uint32_t width, uint32_t height)
{
    for (uint64_t i = 0; i < (uint64_t)width * height; i++)
        pixels[i] = 0;

    struct splash_rect rect;
    if (!splash_fit(splash_width, splash_height, width, height, &rect))
        return;

    for (uint32_t row = 0; row < rect.height; row++) {
        uint32_t fy = splash_coord(row, rect.height, splash_height);
        uint32_t *line = pixels + (uint64_t)(rect.y + row) * width + rect.x;
        for (uint32_t col = 0; col < rect.width; col++) {
            uint32_t fx = splash_coord(col, rect.width, splash_width);
            line[col] = splash_sample(splash_pixels, splash_width, splash_height, fx, fy) *
                        0x010101u;
        }
    }
}

void splash_draw(const struct framebuffer *fb)
{
    gop_fill_rect(fb, 0, 0, fb->width, fb->height, 0, 0, 0);

    struct splash_rect rect;
    if (!splash_fit(splash_width, splash_height, fb->width, fb->height, &rect))
        return;

    /* The image is grayscale, so each pixel format needs only 256 packed values. */
    uint32_t palette[256];
    for (uint32_t i = 0; i < 256; i++)
        palette[i] = pixel_pack(&fb->layout, (uint8_t)i, (uint8_t)i, (uint8_t)i);

    for (uint32_t row = 0; row < rect.height; row++) {
        uint32_t fy = splash_coord(row, rect.height, splash_height);
        volatile uint32_t *line =
            fb->base + (uint64_t)(rect.y + row) * fb->pixels_per_scanline + rect.x;

        for (uint32_t col = 0; col < rect.width; col++) {
            uint32_t fx = splash_coord(col, rect.width, splash_width);
            uint8_t value = splash_sample(splash_pixels, splash_width, splash_height, fx, fy);
            if (value != 0) /* The screen is already black. */
                line[col] = palette[value];
        }
    }
}
