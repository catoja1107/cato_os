/* Software drawing surface. Pixels are 0x00RRGGBB. This code has no firmware dependencies. */
#ifndef CATO_GUI_CANVAS_H
#define CATO_GUI_CANVAS_H

#include <stdint.h>

#define RGB(r, g, b) (((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(b))

/* Text is the 5x7 font drawn at 2x. */
#define FONT_ADVANCE 12
#define FONT_HEIGHT 14
#define FONT_LINE 18

struct rect {
    int x;
    int y;
    int w;
    int h;
};

struct canvas {
    uint32_t *pixels;
    int width;
    int height;
    struct rect clip;
};

int rect_contains(struct rect r, int x, int y);
int rect_intersect(struct rect a, struct rect b, struct rect *out);
struct rect rect_union(struct rect a, struct rect b);

void canvas_init(struct canvas *c, uint32_t *pixels, int width, int height);
void canvas_set_clip(struct canvas *c, struct rect clip);
void canvas_fill(struct canvas *c, struct rect r, uint32_t color);
/* alpha ranges from 0 (unchanged) to 256 (solid color). */
void canvas_blend(struct canvas *c, struct rect r, uint32_t color, int alpha);
void canvas_frame(struct canvas *c, struct rect r, uint32_t color);
/* Copies from a source buffer with the same dimensions as the canvas. */
void canvas_copy(struct canvas *c, struct rect r, const uint32_t *source);
void canvas_text(struct canvas *c, int x, int y, const char *text, uint32_t color);
/* scale 2 draws text twice as large in each direction. */
void canvas_text_scaled(struct canvas *c, int x, int y, const char *text, uint32_t color,
                        int scale);
/* Draws a small picture where '#' is a filled square of scale pixels. */
void canvas_bitmap(struct canvas *c, int x, int y, const char *const *rows, int row_count,
                   int scale, uint32_t color);

#endif
