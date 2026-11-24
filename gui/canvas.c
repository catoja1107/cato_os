#include "canvas.h"

#include "font.h"

int rect_contains(struct rect r, int x, int y)
{
    return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
}

int rect_intersect(struct rect a, struct rect b, struct rect *out)
{
    int x0 = a.x > b.x ? a.x : b.x;
    int y0 = a.y > b.y ? a.y : b.y;
    int x1 = a.x + a.w < b.x + b.w ? a.x + a.w : b.x + b.w;
    int y1 = a.y + a.h < b.y + b.h ? a.y + a.h : b.y + b.h;

    if (x1 <= x0 || y1 <= y0)
        return 0;
    out->x = x0;
    out->y = y0;
    out->w = x1 - x0;
    out->h = y1 - y0;
    return 1;
}

struct rect rect_union(struct rect a, struct rect b)
{
    int x0 = a.x < b.x ? a.x : b.x;
    int y0 = a.y < b.y ? a.y : b.y;
    int x1 = a.x + a.w > b.x + b.w ? a.x + a.w : b.x + b.w;
    int y1 = a.y + a.h > b.y + b.h ? a.y + a.h : b.y + b.h;
    return (struct rect){ x0, y0, x1 - x0, y1 - y0 };
}

void canvas_init(struct canvas *c, uint32_t *pixels, int width, int height)
{
    c->pixels = pixels;
    c->width = width;
    c->height = height;
    c->clip = (struct rect){ 0, 0, width, height };
}

void canvas_set_clip(struct canvas *c, struct rect clip)
{
    struct rect bounds = { 0, 0, c->width, c->height };
    if (!rect_intersect(clip, bounds, &c->clip))
        c->clip = (struct rect){ 0, 0, 0, 0 };
}

void canvas_fill(struct canvas *c, struct rect r, uint32_t color)
{
    struct rect visible;
    if (!rect_intersect(r, c->clip, &visible))
        return;

    for (int y = visible.y; y < visible.y + visible.h; y++) {
        uint32_t *line = c->pixels + (int64_t)y * c->width;
        for (int x = visible.x; x < visible.x + visible.w; x++)
            line[x] = color;
    }
}

void canvas_blend(struct canvas *c, struct rect r, uint32_t color, int alpha)
{
    struct rect visible;
    if (!rect_intersect(r, c->clip, &visible))
        return;

    uint32_t inverse = (uint32_t)(256 - alpha);
    uint32_t red = ((color >> 16) & 0xff) * (uint32_t)alpha;
    uint32_t green = ((color >> 8) & 0xff) * (uint32_t)alpha;
    uint32_t blue = (color & 0xff) * (uint32_t)alpha;

    for (int y = visible.y; y < visible.y + visible.h; y++) {
        uint32_t *line = c->pixels + (int64_t)y * c->width;
        for (int x = visible.x; x < visible.x + visible.w; x++) {
            uint32_t p = line[x];
            line[x] = (((((p >> 16) & 0xff) * inverse + red) >> 8) << 16) |
                      (((((p >> 8) & 0xff) * inverse + green) >> 8) << 8) |
                      (((p & 0xff) * inverse + blue) >> 8);
        }
    }
}

void canvas_frame(struct canvas *c, struct rect r, uint32_t color)
{
    canvas_fill(c, (struct rect){ r.x, r.y, r.w, 1 }, color);
    canvas_fill(c, (struct rect){ r.x, r.y + r.h - 1, r.w, 1 }, color);
    canvas_fill(c, (struct rect){ r.x, r.y, 1, r.h }, color);
    canvas_fill(c, (struct rect){ r.x + r.w - 1, r.y, 1, r.h }, color);
}

void canvas_copy(struct canvas *c, struct rect r, const uint32_t *source)
{
    struct rect visible;
    if (!rect_intersect(r, c->clip, &visible))
        return;

    for (int y = visible.y; y < visible.y + visible.h; y++) {
        uint32_t *to = c->pixels + (int64_t)y * c->width;
        const uint32_t *from = source + (int64_t)y * c->width;
        for (int x = visible.x; x < visible.x + visible.w; x++)
            to[x] = from[x];
    }
}

void canvas_text_scaled(struct canvas *c, int x, int y, const char *text, uint32_t color,
                        int scale)
{
    int dot = 2 * scale;
    int advance = FONT_ADVANCE * scale;
    for (; *text; text++, x += advance) {
        /* The cell includes the extra row used by descending letters. */
        struct rect cell = { x, y, advance, (FONT_HEIGHT + 2) * scale };
        struct rect visible;
        if (!rect_intersect(cell, c->clip, &visible))
            continue;

        const uint8_t *glyph = font_glyph(*text);
        int top = y + (font_descends(*text) ? dot : 0);
        for (int row = 0; row < FONT_GLYPH_ROWS; row++) {
            for (int col = 0; col < FONT_GLYPH_COLS; col++) {
                if (glyph[row] & (0x10 >> col))
                    canvas_fill(c, (struct rect){ x + col * dot, top + row * dot, dot, dot }, color);
            }
        }
    }
}

void canvas_text(struct canvas *c, int x, int y, const char *text, uint32_t color)
{
    canvas_text_scaled(c, x, y, text, color, 1);
}

void canvas_bitmap(struct canvas *c, int x, int y, const char *const *rows, int row_count,
                   int scale, uint32_t color)
{
    for (int row = 0; row < row_count; row++) {
        for (int col = 0; rows[row][col]; col++) {
            if (rows[row][col] == '#')
                canvas_fill(c, (struct rect){ x + col * scale, y + row * scale, scale, scale },
                            color);
        }
    }
}
