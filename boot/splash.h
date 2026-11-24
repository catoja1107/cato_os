/*
 * Boot splash. The geometry and sampling helpers are pure so that the host
 * unit tests can exercise them.
 */
#ifndef CATO_BOOT_SPLASH_H
#define CATO_BOOT_SPLASH_H

#include <stdint.h>

struct framebuffer;

struct splash_rect {
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
};

/* Clears the whole screen, including any vendor logo, and draws the splash. */
void splash_draw(const struct framebuffer *fb);

/* Renders the splash on black into a 0x00RRGGBB buffer, for use as the wallpaper. */
void splash_render(uint32_t *pixels, uint32_t width, uint32_t height);

/* Centers the image in 92% of the screen, preserving its aspect ratio. */
static inline int splash_fit(uint32_t src_width, uint32_t src_height,
                             uint32_t screen_width, uint32_t screen_height,
                             struct splash_rect *out)
{
    if (src_width == 0 || src_height == 0)
        return 0;

    uint64_t max_width = (uint64_t)screen_width * 92 / 100;
    uint64_t max_height = (uint64_t)screen_height * 92 / 100;
    uint64_t width = max_width;
    uint64_t height = width * src_height / src_width;
    if (height > max_height) {
        height = max_height;
        width = height * src_width / src_height;
    }
    if (width == 0 || height == 0)
        return 0;

    out->x = (uint32_t)((screen_width - width) / 2);
    out->y = (uint32_t)((screen_height - height) / 2);
    out->width = (uint32_t)width;
    out->height = (uint32_t)height;
    return 1;
}

/* Maps a destination pixel's center to a 16.16 fixed-point source coordinate. */
static inline uint32_t splash_coord(uint32_t dest, uint32_t dest_size, uint32_t src_size)
{
    uint64_t center = (((uint64_t)dest * 2 + 1) * ((uint64_t)src_size << 16)) /
                      ((uint64_t)dest_size * 2);
    uint64_t max = (uint64_t)(src_size - 1) << 16;

    if (center <= 0x8000)
        return 0;
    center -= 0x8000;
    return (uint32_t)(center > max ? max : center);
}

/* Bilinear sample at a 16.16 fixed-point source coordinate. */
static inline uint8_t splash_sample(const uint8_t *pixels, uint32_t width, uint32_t height,
                                    uint32_t fx, uint32_t fy)
{
    uint32_t x0 = fx >> 16;
    uint32_t y0 = fy >> 16;
    uint32_t x1 = x0 + 1 < width ? x0 + 1 : x0;
    uint32_t y1 = y0 + 1 < height ? y0 + 1 : y0;
    uint32_t tx = (fx >> 8) & 0xff;
    uint32_t ty = (fy >> 8) & 0xff;

    const uint8_t *row0 = pixels + (uint64_t)y0 * width;
    const uint8_t *row1 = pixels + (uint64_t)y1 * width;
    uint32_t top = row0[x0] * (256 - tx) + row0[x1] * tx;
    uint32_t bottom = row1[x0] * (256 - tx) + row1[x1] * tx;
    return (uint8_t)((top * (256 - ty) + bottom * ty + 32768) >> 16);
}

#endif
