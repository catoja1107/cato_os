#ifndef CATO_BOOT_GOP_H
#define CATO_BOOT_GOP_H

#include "efi.h"
#include "pixel.h"

struct framebuffer {
    volatile uint32_t *base;
    uint64_t phys_base;
    uint64_t size;
    uint32_t width;
    uint32_t height;
    uint32_t pixels_per_scanline; /* Can exceed width. Never assume they match. */
    EFI_GRAPHICS_PIXEL_FORMAT format;
    struct pixel_layout layout;
};

/* Finds a GOP with a linear framebuffer, keeping the current video mode. */
EFI_STATUS gop_open(EFI_SYSTEM_TABLE *st, struct framebuffer *fb);

void gop_fill_rect(const struct framebuffer *fb, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                   uint8_t red, uint8_t green, uint8_t blue);

/* Draws seven vertical color bars across the lower part of the screen. */
void gop_draw_test_pattern(const struct framebuffer *fb);

#endif
