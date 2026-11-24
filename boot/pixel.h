/*
 * Pure pixel-format logic, shared by the loader and the host unit tests.
 *
 * GOP reports either a fixed 32-bit RGB/BGR layout or per-channel bit masks
 * (UEFI Specification, "Graphics Output Protocol", EFI_PIXEL_BITMASK).
 */
#ifndef CATO_BOOT_PIXEL_H
#define CATO_BOOT_PIXEL_H

#include <stdint.h>

struct pixel_channel {
    uint8_t shift;
    uint8_t bits;
};

struct pixel_layout {
    struct pixel_channel red;
    struct pixel_channel green;
    struct pixel_channel blue;
};

/* A valid channel mask is one non-empty, contiguous run of set bits. */
static inline int pixel_channel_from_mask(uint32_t mask, struct pixel_channel *out)
{
    if (mask == 0)
        return 0;

    unsigned shift = (unsigned)__builtin_ctz(mask);
    uint64_t run = (uint64_t)mask >> shift;
    unsigned bits = 0;
    while (run & 1) {
        bits++;
        run >>= 1;
    }
    if (run != 0)
        return 0;

    out->shift = (uint8_t)shift;
    out->bits = (uint8_t)bits;
    return 1;
}

/*
 * Builds a layout from GOP masks. Returns 0 for masks that are invalid or
 * overlap.
 *
 * Deliberate shortcut: only 32-bit pixels are supported, so the highest set
 * bit across all four masks must be in byte 3.
 */
static inline int pixel_layout_from_masks(uint32_t red, uint32_t green, uint32_t blue,
                                          uint32_t reserved, struct pixel_layout *out)
{
    if ((red & green) || (red & blue) || (green & blue) ||
        (reserved & (red | green | blue)))
        return 0;
    if ((red | green | blue | reserved) < (1u << 24))
        return 0;

    return pixel_channel_from_mask(red, &out->red) &&
           pixel_channel_from_mask(green, &out->green) &&
           pixel_channel_from_mask(blue, &out->blue);
}

/* Scales an 8-bit component to the channel width, rounding to nearest. */
static inline uint32_t pixel_scale(uint8_t value, struct pixel_channel channel)
{
    uint64_t max = (1ULL << channel.bits) - 1;
    uint64_t scaled = ((uint64_t)value * max + 127) / 255;
    return (uint32_t)(scaled << channel.shift);
}

static inline uint32_t pixel_pack(const struct pixel_layout *layout,
                                  uint8_t red, uint8_t green, uint8_t blue)
{
    return pixel_scale(red, layout->red) |
           pixel_scale(green, layout->green) |
           pixel_scale(blue, layout->blue);
}

#endif
