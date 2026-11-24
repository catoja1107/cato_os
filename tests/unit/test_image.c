/*
 * Host tests for the PNG, JPEG, and GIF decoders. Expected pixels come from
 * tests/gen_test_images.py, which uses Pillow as an independent reference.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "image.h"

#define BACKGROUND 0x1e1e25u
#define MAX_FILE (1 << 20)
#define MAX_PIXELS (1 << 22)

static int failures;
static const char *g_directory;
static uint8_t g_file[MAX_FILE];
static uint8_t g_mutated[MAX_FILE];
static uint8_t g_expected[16 + MAX_PIXELS * 3];
static uint32_t g_pixels[MAX_PIXELS];

#define CHECK(condition)                                                 \
    do {                                                                 \
        if (!(condition)) {                                              \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);  \
            failures++;                                                  \
        }                                                                \
    } while (0)

static int load(const char *name, uint8_t *buffer, int capacity)
{
    char path[512];
    snprintf(path, sizeof path, "%s/%s", g_directory, name);
    FILE *file = fopen(path, "rb");
    if (!file) {
        printf("FAIL missing test file %s\n", path);
        failures++;
        return 0;
    }
    int length = (int)fread(buffer, 1, (size_t)capacity, file);
    fclose(file);
    return length;
}

static struct image_request full_size_request(void)
{
    struct image_request request = { 4096, 4096, 0, 0, BACKGROUND, g_pixels, MAX_PIXELS };
    return request;
}

/* Decodes NAME.EXT at full size and compares it with NAME.rgb. */
static void check_exact(const char *file, const char *expected, int mean_limit, int max_limit)
{
    int length = load(file, g_file, MAX_FILE);
    int expected_length = load(expected, g_expected, sizeof g_expected);
    if (!length || expected_length < 8)
        return;

    struct image_request request = full_size_request();
    struct image_result result;
    int status = image_decode(g_file, length, &request, &result);
    if (status != IMAGE_OK) {
        printf("FAIL %s: %s\n", file, image_status_text(status));
        failures++;
        return;
    }

    int width = (int)(g_expected[0] | g_expected[1] << 8 | g_expected[2] << 16);
    int height = (int)(g_expected[4] | g_expected[5] << 8 | g_expected[6] << 16);
    if (result.width != width || result.height != height) {
        printf("FAIL %s: size %dx%d, expected %dx%d\n", file, result.width, result.height, width,
               height);
        failures++;
        return;
    }

    long total = 0;
    int worst = 0;
    for (int i = 0; i < width * height; i++) {
        const uint8_t *e = g_expected + 8 + i * 3;
        uint32_t p = g_pixels[i];
        int got[3] = { (int)(p >> 16) & 255, (int)(p >> 8) & 255, (int)p & 255 };
        for (int c = 0; c < 3; c++) {
            int difference = abs(got[c] - e[c]);
            total += difference;
            if (difference > worst)
                worst = difference;
        }
    }
    int mean_x100 = (int)(total * 100 / (width * height * 3));
    if (mean_x100 > mean_limit * 100 || worst > max_limit) {
        printf("FAIL %s: mean error %d.%02d, worst %d\n", file, mean_x100 / 100, mean_x100 % 100,
               worst);
        failures++;
    }
}

static void test_reference_images(void)
{
    /* Lossless formats must match exactly. */
    check_exact("rgb8.png", "rgb8.rgb", 0, 0);
    check_exact("rgba8.png", "rgba8.rgb", 0, 0);
    check_exact("gray8.png", "gray8.rgb", 0, 0);
    check_exact("grayalpha8.png", "grayalpha8.rgb", 0, 0);
    check_exact("gray16.png", "gray16.rgb", 0, 0);
    check_exact("gray1.png", "gray1.rgb", 0, 0);
    check_exact("palette4.png", "palette4.rgb", 0, 0);
    check_exact("palette8.png", "palette8.rgb", 0, 0);
    check_exact("plain.gif", "plain.rgb", 0, 0);
    check_exact("interlaced.gif", "interlaced.rgb", 0, 0);

    /* JPEG decoders legitimately differ slightly, especially in chroma upsampling. */
    check_exact("baseline444.jpg", "baseline444.rgb", 2, 24);
    check_exact("gray.jpg", "gray.rgb", 2, 16);
    check_exact("baseline420.jpg", "baseline420.rgb", 2, 24);
}

static void test_unsupported(void)
{
    struct image_request request = full_size_request();
    struct image_result result;
    int length = load("progressive.jpg", g_file, MAX_FILE);
    CHECK(image_decode(g_file, length, &request, &result) == IMAGE_UNSUPPORTED);
    length = load("not-an-image.webp", g_file, MAX_FILE);
    CHECK(image_decode(g_file, length, &request, &result) == IMAGE_UNSUPPORTED);
    CHECK(image_decode((const uint8_t *)"", 0, &request, &result) == IMAGE_UNSUPPORTED);
}

static void test_scaling(void)
{
    struct image_request request = full_size_request();
    struct image_result result;
    int length = load("rgb8.png", g_file, MAX_FILE);

    /* Bounded by the display width, keeping the aspect ratio. */
    request.max_width = 40;
    CHECK(image_decode(g_file, length, &request, &result) == IMAGE_OK);
    CHECK(result.width == 40 && result.height == 25);

    /* HTML attributes set the size, but never enlarge. */
    request = full_size_request();
    request.want_width = 50;
    CHECK(image_decode(g_file, length, &request, &result) == IMAGE_OK);
    CHECK(result.width == 50 && result.height == 31);
    request.want_width = 500;
    request.want_height = 0;
    CHECK(image_decode(g_file, length, &request, &result) == IMAGE_OK);
    CHECK(result.width == 97 && result.height == 61);

    /* The output never exceeds the buffer it was given. */
    request = full_size_request();
    request.capacity = 100;
    CHECK(image_decode(g_file, length, &request, &result) == IMAGE_OK);
    CHECK(result.width * result.height <= 100);

    /* A scaled image is an average of the full one, so its corners keep their colors. */
    request = full_size_request();
    request.max_width = 10;
    CHECK(image_decode(g_file, length, &request, &result) == IMAGE_OK);
    CHECK(((g_pixels[0] >> 16) & 255) < 40);
    CHECK(((g_pixels[result.width - 1] >> 16) & 255) > 215);
}

static void test_damaged(void)
{
    static const char *const FILES[] = {
        "rgb8.png", "palette4.png", "baseline420.jpg", "baseline444.jpg", "gray.jpg",
        "plain.gif", "interlaced.gif", "rgba8.png", "gray16.png",
    };
    uint32_t random = 0x9e3779b9u;

    for (unsigned f = 0; f < sizeof FILES / sizeof FILES[0]; f++) {
        int length = load(FILES[f], g_file, MAX_FILE);
        if (!length)
            continue;

        /* Every truncation fails cleanly or still decodes. */
        for (int cut = 0; cut < length; cut += length / 97 + 1) {
            struct image_request request = full_size_request();
            struct image_result result;
            memcpy(g_mutated, g_file, (size_t)cut);
            int status = image_decode(g_mutated, cut, &request, &result);
            CHECK(status == IMAGE_OK || status < 0);
        }

        /* Random corruption never escapes the buffers (checked by AddressSanitizer). */
        for (int round = 0; round < 400; round++) {
            memcpy(g_mutated, g_file, (size_t)length);
            int edits = 1 + round % 8;
            for (int e = 0; e < edits; e++) {
                random ^= random << 13;
                random ^= random >> 17;
                random ^= random << 5;
                g_mutated[random % (uint32_t)length] = (uint8_t)(random >> 24);
            }
            struct image_request request = full_size_request();
            request.capacity = 50000;
            struct image_result result;
            int status = image_decode(g_mutated, length, &request, &result);
            if (status == IMAGE_OK)
                CHECK(result.width * result.height <= 50000);
        }
    }
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        puts("usage: test_image <directory>");
        return 2;
    }
    g_directory = argv[1];
    test_reference_images();
    test_unsupported();
    test_scaling();
    test_damaged();

    if (failures) {
        printf("%d image check(s) failed\n", failures);
        return 1;
    }
    puts("image unit tests passed");
    return 0;
}
