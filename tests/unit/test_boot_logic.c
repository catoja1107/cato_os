/* Host unit tests for the loader's pure logic. Built with ASan and UBSan. */
#include <stdio.h>

#include "fmt.h"
#include "pixel.h"
#include "splash.h"

static int failures;

#define CHECK(condition)                                                 \
    do {                                                                 \
        if (!(condition)) {                                              \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);  \
            failures++;                                                  \
        }                                                                \
    } while (0)

static int u16_equals(const uint16_t *actual, const char *expected)
{
    size_t i = 0;
    for (; expected[i]; i++) {
        if (actual[i] != (uint16_t)expected[i])
            return 0;
    }
    return actual[i] == 0;
}

static void test_channel_masks(void)
{
    struct pixel_channel channel;

    CHECK(pixel_channel_from_mask(0x00ff0000, &channel));
    CHECK(channel.shift == 16 && channel.bits == 8);
    CHECK(pixel_channel_from_mask(0xffffffff, &channel));
    CHECK(channel.shift == 0 && channel.bits == 32);
    CHECK(pixel_channel_from_mask(0x80000000, &channel));
    CHECK(channel.shift == 31 && channel.bits == 1);
    CHECK(!pixel_channel_from_mask(0, &channel));
    CHECK(!pixel_channel_from_mask(0x00ff00ff, &channel));
}

static void test_layouts(void)
{
    struct pixel_layout layout;

    CHECK(pixel_layout_from_masks(0x00ff0000, 0x0000ff00, 0x000000ff, 0xff000000, &layout));
    CHECK(pixel_pack(&layout, 0x12, 0x34, 0x56) == 0x00123456);

    CHECK(pixel_layout_from_masks(0x000000ff, 0x0000ff00, 0x00ff0000, 0xff000000, &layout));
    CHECK(pixel_pack(&layout, 0x12, 0x34, 0x56) == 0x00563412);

    /* A2R10G10B10: full-scale 8-bit input must reach full-scale 10-bit output. */
    CHECK(pixel_layout_from_masks(0x3ff00000, 0x000ffc00, 0x000003ff, 0xc0000000, &layout));
    CHECK(pixel_pack(&layout, 255, 255, 255) == 0x3fffffff);
    CHECK(pixel_pack(&layout, 0, 0, 0) == 0);

    /* Overlapping channels, reserved overlap, and 24-bit pixels are rejected. */
    CHECK(!pixel_layout_from_masks(0x00ff0000, 0x00ff0000, 0x000000ff, 0xff000000, &layout));
    CHECK(!pixel_layout_from_masks(0x00ff0000, 0x0000ff00, 0x000000ff, 0x000000ff, &layout));
    CHECK(!pixel_layout_from_masks(0x00ff0000, 0x0000ff00, 0x000000ff, 0, &layout));
}

static void test_format(void)
{
    uint16_t buffer[32];

    CHECK(fmt_u64(buffer, 32, 0, 10, 1) == 1 && u16_equals(buffer, "0"));
    CHECK(fmt_u64(buffer, 32, 3840, 10, 1) == 4 && u16_equals(buffer, "3840"));
    CHECK(fmt_u64(buffer, 32, 0xbeef, 16, 8) == 8 && u16_equals(buffer, "0000beef"));
    CHECK(fmt_u64(buffer, 32, UINT64_MAX, 16, 16) == 16 && u16_equals(buffer, "ffffffffffffffff"));

    /* Capacity must include the terminator. */
    CHECK(fmt_u64(buffer, 4, 1234, 10, 1) == 0 && buffer[0] == 0);
    CHECK(fmt_u64(buffer, 5, 1234, 10, 1) == 4 && u16_equals(buffer, "1234"));

    CHECK(fmt_u64(buffer, 32, 1, 1, 1) == 0);
    CHECK(fmt_u64(buffer, 32, 1, 17, 1) == 0);
    CHECK(fmt_u64(buffer, 32, 1, 10, 65) == 0);
}

static void test_splash_fit(void)
{
    struct splash_rect rect;

    CHECK(splash_fit(700, 600, 1280, 800, &rect));
    CHECK(rect.x == 211 && rect.y == 32 && rect.width == 858 && rect.height == 736);

    CHECK(splash_fit(700, 600, 3840, 2160, &rect));
    CHECK(rect.x == 761 && rect.y == 86 && rect.width == 2318 && rect.height == 1987);

    /* A portrait screen is limited by width instead of height. */
    CHECK(splash_fit(700, 600, 600, 1000, &rect));
    CHECK(rect.x == 24 && rect.y == 263 && rect.width == 552 && rect.height == 473);

    CHECK(!splash_fit(0, 600, 1280, 800, &rect));
    CHECK(!splash_fit(700, 0, 1280, 800, &rect));
    CHECK(!splash_fit(700, 600, 1, 1, &rect));
}

static void test_splash_sampling(void)
{
    static const uint8_t pixels[4] = { 0, 100, 200, 255 };

    /* At 1:1 scale, destination pixels land exactly on source pixels. */
    CHECK(splash_coord(0, 4, 4) == 0);
    CHECK(splash_coord(1, 4, 4) == 1u << 16);
    CHECK(splash_coord(3, 4, 4) == 3u << 16);

    /* Upscaled coordinates stay inside the source image. */
    CHECK(splash_coord(0, 4000, 2) == 0);
    CHECK(splash_coord(3999, 4000, 2) == 1u << 16);

    CHECK(splash_sample(pixels, 2, 2, 0, 0) == 0);
    CHECK(splash_sample(pixels, 2, 2, 1u << 16, 0) == 100);
    CHECK(splash_sample(pixels, 2, 2, 0, 1u << 16) == 200);
    CHECK(splash_sample(pixels, 2, 2, 1u << 16, 1u << 16) == 255);
    CHECK(splash_sample(pixels, 2, 2, 0x8000, 0) == 50);
}

int main(void)
{
    test_channel_masks();
    test_layouts();
    test_format();
    test_splash_fit();
    test_splash_sampling();

    if (failures) {
        printf("%d unit check(s) failed\n", failures);
        return 1;
    }
    puts("unit tests passed");
    return 0;
}
