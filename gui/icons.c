#include "icons.h"

#define FOLDER_BACK RGB(190, 138, 48)
#define FOLDER_FRONT RGB(232, 176, 75)
#define PAPER RGB(232, 230, 225)
#define PAPER_LINE RGB(138, 136, 146)
#define METAL RGB(150, 150, 160)
#define METAL_DARK RGB(96, 96, 106)
#define SCREEN_DARK RGB(14, 14, 18)
#define SCREEN_EDGE RGB(170, 170, 180)
#define ACCENT RGB(232, 176, 75)

struct pen {
    struct canvas *canvas;
    int x;
    int y;
    int scale;
};

static void part(const struct pen *p, int x, int y, int w, int h, uint32_t color)
{
    canvas_fill(p->canvas,
                (struct rect){ p->x + x * p->scale, p->y + y * p->scale, w * p->scale,
                               h * p->scale },
                color);
}

static void draw_folder(const struct pen *p)
{
    part(p, 1, 3, 6, 2, FOLDER_BACK);
    part(p, 1, 5, 14, 9, FOLDER_BACK);
    part(p, 1, 6, 14, 8, FOLDER_FRONT);
}

static void draw_file(const struct pen *p)
{
    part(p, 3, 1, 8, 14, PAPER);
    part(p, 11, 4, 2, 11, PAPER);
    part(p, 11, 2, 1, 2, PAPER_LINE);
    part(p, 5, 6, 6, 1, PAPER_LINE);
    part(p, 5, 9, 6, 1, PAPER_LINE);
    part(p, 5, 12, 4, 1, PAPER_LINE);
}

static void draw_trash(const struct pen *p)
{
    part(p, 6, 1, 4, 1, METAL);
    part(p, 2, 2, 12, 2, METAL);
    part(p, 3, 5, 10, 10, METAL);
    part(p, 5, 6, 1, 8, METAL_DARK);
    part(p, 8, 6, 1, 8, METAL_DARK);
    part(p, 11, 6, 1, 8, METAL_DARK);
}

static void draw_terminal(const struct pen *p)
{
    part(p, 1, 2, 14, 12, SCREEN_EDGE);
    part(p, 2, 3, 12, 10, SCREEN_DARK);
    part(p, 4, 5, 1, 1, ACCENT);
    part(p, 5, 6, 1, 1, ACCENT);
    part(p, 6, 7, 1, 1, ACCENT);
    part(p, 5, 8, 1, 1, ACCENT);
    part(p, 4, 9, 1, 1, ACCENT);
    part(p, 8, 10, 4, 1, ACCENT);
}

static void draw_editor(const struct pen *p)
{
    draw_file(p);
    part(p, 12, 3, 2, 2, ACCENT);
    part(p, 11, 5, 2, 2, ACCENT);
    part(p, 10, 7, 2, 2, ACCENT);
    part(p, 9, 9, 2, 2, ACCENT);
    part(p, 8, 11, 1, 1, METAL_DARK);
}

static void draw_system(const struct pen *p)
{
    part(p, 1, 2, 14, 10, SCREEN_EDGE);
    part(p, 2, 3, 12, 8, SCREEN_DARK);
    part(p, 4, 5, 8, 1, ACCENT);
    part(p, 4, 8, 5, 1, PAPER_LINE);
    part(p, 7, 12, 2, 2, SCREEN_EDGE);
    part(p, 4, 14, 8, 1, SCREEN_EDGE);
}

static void draw_browser(const struct pen *p)
{
    static const uint8_t SPANS[14][2] = {
        { 5, 6 }, { 3, 10 }, { 2, 12 }, { 2, 12 }, { 1, 14 }, { 1, 14 }, { 1, 14 },
        { 1, 14 }, { 1, 14 }, { 1, 14 }, { 2, 12 }, { 2, 12 }, { 3, 10 }, { 5, 6 },
    };
    for (int row = 0; row < 14; row++)
        part(p, SPANS[row][0], row + 1, SPANS[row][1], 1, RGB(74, 130, 200));
    part(p, 7, 1, 2, 14, RGB(170, 205, 245));
    part(p, 1, 7, 14, 2, RGB(170, 205, 245));
    part(p, 4, 3, 8, 1, RGB(170, 205, 245));
    part(p, 4, 12, 8, 1, RGB(170, 205, 245));
}

void icon_draw(struct canvas *c, int kind, int x, int y, int scale)
{
    struct pen pen = { c, x, y, scale };
    switch (kind) {
    case ICON_BROWSER:
        draw_browser(&pen);
        break;
    case ICON_FOLDER:
        draw_folder(&pen);
        break;
    case ICON_TRASH:
        draw_trash(&pen);
        break;
    case ICON_TERMINAL:
        draw_terminal(&pen);
        break;
    case ICON_EDITOR:
        draw_editor(&pen);
        break;
    case ICON_SYSTEM:
        draw_system(&pen);
        break;
    default:
        draw_file(&pen);
        break;
    }
}
