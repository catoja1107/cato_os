#include "wm.h"

#include "apps.h"
#include "icons.h"
#include "shell.h"
#include "text.h"

#define SHADOW 6
#define BUTTON_SIZE 18
#define BUTTON_STEP 22
#define GRIP_SIZE 14
#define START_W 96
#define TASK_X 108
#define TASK_W 136
#define TASK_GAP 4
#define CLOCK_W 212
#define MENU_W 240
#define MENU_ITEM_H 30
#define MENU_ITEMS 7
#define POINTER_W 11
#define POINTER_H 16
#define MOVE_STEP 16
#define DOUBLE_CLICK_TICKS 32
#define DOUBLE_CLICK_DISTANCE 4

static const char *const MENU_LABELS[MENU_ITEMS] = {
    "Files", "Text Editor", "Terminal", "System", "Web Browser", "Show Desktop", "Exit to firmware",
};

/* '#' is the outline and '.' is the fill. */
static const char *const POINTER_SHAPE[POINTER_H] = {
    "#          ",
    "##         ",
    "#.#        ",
    "#..#       ",
    "#...#      ",
    "#....#     ",
    "#.....#    ",
    "#......#   ",
    "#.......#  ",
    "#........# ",
    "#.....#####",
    "#..#..#    ",
    "#.# #..#   ",
    "##  #..#   ",
    "#    #..#  ",
    "     ####  ",
};

static const char *const CAT_ICON[6] = {
    "#     #",
    "##   ##",
    "#######",
    "# ### #",
    "#######",
    " ##### ",
};

static int work_height(const struct wm *wm)
{
    return wm->screen->height - WM_TASKBAR_H;
}

static struct rect taskbar_rect(const struct wm *wm)
{
    return (struct rect){ 0, work_height(wm), wm->screen->width, WM_TASKBAR_H };
}

static struct rect start_rect(const struct wm *wm)
{
    return (struct rect){ 4, work_height(wm) + 4, START_W, WM_TASKBAR_H - 8 };
}

static struct rect task_rect(const struct wm *wm, int ordinal)
{
    int width = (wm->screen->width - TASK_X - CLOCK_W) / APP_COUNT - TASK_GAP;
    if (width > TASK_W)
        width = TASK_W;
    return (struct rect){ TASK_X + ordinal * (width + TASK_GAP), work_height(wm) + 4, width,
                          WM_TASKBAR_H - 8 };
}

static struct rect menu_rect(const struct wm *wm)
{
    int height = MENU_ITEMS * MENU_ITEM_H + 8;
    return (struct rect){ 4, work_height(wm) - height, MENU_W, height };
}

static struct rect menu_item_rect(const struct wm *wm, int index)
{
    struct rect menu = menu_rect(wm);
    return (struct rect){ menu.x + 4, menu.y + 4 + index * MENU_ITEM_H, menu.w - 8, MENU_ITEM_H };
}

static struct rect window_bounds(const struct wm *wm, int app)
{
    struct rect r = wm->windows[app].frame;
    r.w += SHADOW;
    r.h += SHADOW;
    return r;
}

static struct rect title_button(struct rect frame, int slot)
{
    return (struct rect){ frame.x + frame.w - BUTTON_SIZE - 5 - slot * BUTTON_STEP, frame.y + 5,
                          BUTTON_SIZE, BUTTON_SIZE };
}

static int minimize_slot(int app)
{
    return app_resizable(app) ? 2 : 1;
}

static struct rect grip_rect(struct rect frame)
{
    return (struct rect){ frame.x + frame.w - GRIP_SIZE, frame.y + frame.h - GRIP_SIZE, GRIP_SIZE,
                          GRIP_SIZE };
}

static struct rect pointer_rect(const struct wm *wm)
{
    return (struct rect){ wm->pointer_x, wm->pointer_y, POINTER_W, POINTER_H };
}

static int clamp(int value, int low, int high)
{
    if (value > high)
        value = high;
    return value < low ? low : value;
}

/* Keeps enough of the title bar on screen for the window to stay reachable. */
static void clamp_frame(const struct wm *wm, struct rect *frame)
{
    frame->x = clamp(frame->x, 60 - frame->w, wm->screen->width - 60);
    frame->y = clamp(frame->y, 0, work_height(wm) - WM_TITLE_H);
}

void wm_trace(const struct wm *wm, const char *event)
{
    if (wm->trace)
        wm->trace(event);
}

static void trace_app(const struct wm *wm, const char *verb, int app)
{
    char event[48];
    text_copy(event, sizeof event, verb);
    text_append(event, sizeof event, app_slug(app));
    wm_trace(wm, event);
}

static void trace_frame(const struct wm *wm, const char *verb, int app)
{
    const struct rect f = wm->windows[app].frame;
    char event[64];
    text_copy(event, sizeof event, verb);
    text_append(event, sizeof event, app_slug(app));
    text_append(event, sizeof event, ":");
    text_append_u64(event, sizeof event, (uint64_t)(f.x < 0 ? 0 : f.x));
    text_append(event, sizeof event, ",");
    text_append_u64(event, sizeof event, (uint64_t)(f.y < 0 ? 0 : f.y));
    text_append(event, sizeof event, ",");
    text_append_u64(event, sizeof event, (uint64_t)f.w);
    text_append(event, sizeof event, ",");
    text_append_u64(event, sizeof event, (uint64_t)f.h);
    wm_trace(wm, event);
}

void wm_invalidate(struct wm *wm, struct rect r)
{
    struct rect bounds = { 0, 0, wm->screen->width, wm->screen->height };
    struct rect clipped;
    if (!rect_intersect(r, bounds, &clipped))
        return;

    for (int i = 0; i < wm->dirty_count; i++) {
        struct rect overlap;
        if (rect_intersect(wm->dirty[i], clipped, &overlap)) {
            wm->dirty[i] = rect_union(wm->dirty[i], clipped);
            return;
        }
    }
    if (wm->dirty_count < WM_MAX_DIRTY) {
        wm->dirty[wm->dirty_count++] = clipped;
        return;
    }

    /* The list is full, so collapse it into one rectangle. */
    struct rect all = clipped;
    for (int i = 0; i < wm->dirty_count; i++)
        all = rect_union(all, wm->dirty[i]);
    wm->dirty[0] = all;
    wm->dirty_count = 1;
}

void wm_invalidate_app(struct wm *wm, int app)
{
    if (wm->windows[app].open)
        wm_invalidate(wm, window_bounds(wm, app));
}

void wm_invalidate_all(struct wm *wm)
{
    wm_invalidate(wm, (struct rect){ 0, 0, wm->screen->width, wm->screen->height });
}

void wm_init(struct wm *wm, struct canvas *screen, const uint32_t *wallpaper,
             const struct sysinfo *info, struct fs *fs, void (*trace)(const char *event))
{
    wm->screen = screen;
    wm->wallpaper = wallpaper;
    wm->info = info;
    wm->fs = fs;
    wm->net = 0;
    wm->storage_ready = 0;
    wm->trace = trace;
    for (int i = 0; i < APP_COUNT; i++) {
        wm->windows[i].open = 0;
        wm->windows[i].minimized = 0;
        wm->windows[i].maximized = 0;
    }
    wm->z_count = 0;
    wm->desktop_focus = 0;

    terminal_init(&wm->terminal);
    files_init(wm);
    editor_reset(wm);
    browser_init(wm);
    wm->clipboard.node = FS_NONE;
    wm->clipboard.cut = 0;
    wm->context.open = 0;
    wm->dialog.kind = DIALOG_NONE;
    wm->icon_selected = -1;
    text_copy(wm->clock, sizeof wm->clock, "---------- --:--");

    wm->menu_open = 0;
    wm->menu_index = 0;
    wm->move_mode = 0;
    wm->pointer_x = screen->width / 2;
    wm->pointer_y = screen->height / 2;
    wm->pointer_visible = 0;
    wm->buttons = 0;
    wm->drag_app = -1;
    wm->drag_resize = 0;
    wm->drag_dx = 0;
    wm->drag_dy = 0;
    wm->ticks = 0;
    wm->last_click = 0;
    wm->click_armed = 0;
    wm->click_x = 0;
    wm->click_y = 0;
    wm->exit_requested = 0;
    wm->dirty_count = 0;
    wm_invalidate_all(wm);
}

int wm_focused(const struct wm *wm)
{
    if (wm->desktop_focus)
        return -1;
    for (int i = wm->z_count - 1; i >= 0; i--) {
        if (!wm->windows[wm->z[i]].minimized)
            return wm->z[i];
    }
    return -1;
}

struct rect wm_content_rect(const struct wm *wm, int app)
{
    struct rect f = wm->windows[app].frame;
    return (struct rect){ f.x + 1, f.y + WM_TITLE_H, f.w - 2, f.h - WM_TITLE_H - 1 };
}

static void remove_from_z(struct wm *wm, int app)
{
    int out = 0;
    for (int i = 0; i < wm->z_count; i++) {
        if (wm->z[i] != app)
            wm->z[out++] = wm->z[i];
    }
    wm->z_count = out;
}

void wm_focus(struct wm *wm, int app)
{
    struct window *window = &wm->windows[app];
    int previous = wm_focused(wm);
    if (!window->open || previous == app)
        return;

    window->minimized = 0;
    wm->desktop_focus = 0;
    remove_from_z(wm, app);
    wm->z[wm->z_count++] = app;
    if (previous >= 0)
        wm_invalidate_app(wm, previous);
    wm_invalidate_app(wm, app);
    wm_invalidate(wm, taskbar_rect(wm));
    trace_app(wm, "focus:", app);
}

void wm_focus_desktop(struct wm *wm)
{
    int previous = wm_focused(wm);
    wm->desktop_focus = 1;
    if (previous >= 0)
        wm_invalidate_app(wm, previous);
    wm_invalidate(wm, taskbar_rect(wm));
}

static struct rect default_frame(const struct wm *wm, int app)
{
    int width = wm->screen->width;
    int height = work_height(wm);
    struct rect f;

    app_default_size(app, &f.w, &f.h);
    if (app_resizable(app)) {
        f.w = f.w > width ? width : f.w;
        f.h = f.h > height ? height : f.h;
    }

    switch (app) {
    case APP_FILES:
        f.x = (width - f.w) / 2;
        f.y = (height - f.h) / 3;
        break;
    case APP_EDITOR:
        f.x = (width - f.w) / 2 + 48;
        f.y = (height - f.h) / 3 + 40;
        break;
    case APP_TERMINAL:
        f.x = width - f.w - 24;
        f.y = height - f.h - 16;
        break;
    default:
        f.x = width - f.w - 24;
        f.y = 24;
        break;
    }
    clamp_frame(wm, &f);
    return f;
}

void wm_open(struct wm *wm, int app)
{
    struct window *window = &wm->windows[app];
    if (window->open) {
        wm_focus(wm, app);
        return;
    }

    int previous = wm_focused(wm);
    window->frame = default_frame(wm, app);
    window->restore = window->frame;
    window->open = 1;
    window->minimized = 0;
    window->maximized = 0;
    wm->desktop_focus = 0;
    wm->z[wm->z_count++] = app;
    if (previous >= 0)
        wm_invalidate_app(wm, previous);
    wm_invalidate_app(wm, app);
    wm_invalidate(wm, taskbar_rect(wm));
    trace_frame(wm, "open:", app);
}

void wm_close(struct wm *wm, int app)
{
    if (!wm->windows[app].open)
        return;

    wm_invalidate_app(wm, app);
    wm_invalidate(wm, taskbar_rect(wm));
    wm->windows[app].open = 0;
    wm->windows[app].minimized = 0;
    wm->windows[app].maximized = 0;
    remove_from_z(wm, app);
    if (wm->drag_app == app)
        wm->drag_app = -1;
    wm->move_mode = 0;
    if (app == APP_EDITOR)
        editor_reset(wm);

    int focused = wm_focused(wm);
    if (focused >= 0)
        wm_invalidate_app(wm, focused);
    trace_app(wm, "close:", app);
}

void wm_request_close(struct wm *wm, int app)
{
    if (app == APP_EDITOR && wm->editor.dirty)
        editor_prompt_unsaved(wm, FS_NONE);
    else
        wm_close(wm, app);
}

void wm_minimize(struct wm *wm, int app)
{
    if (!wm->windows[app].open || wm->windows[app].minimized)
        return;

    wm_invalidate_app(wm, app);
    wm->windows[app].minimized = 1;
    if (wm->drag_app == app)
        wm->drag_app = -1;
    wm->move_mode = 0;
    wm_invalidate(wm, taskbar_rect(wm));

    int focused = wm_focused(wm);
    if (focused >= 0)
        wm_invalidate_app(wm, focused);
    trace_app(wm, "minimize:", app);
}

void wm_toggle_maximize(struct wm *wm, int app)
{
    struct window *window = &wm->windows[app];
    if (!window->open || !app_resizable(app))
        return;

    wm_invalidate_app(wm, app);
    if (window->maximized) {
        window->frame = window->restore;
        window->maximized = 0;
    } else {
        window->restore = window->frame;
        window->frame = (struct rect){ 0, 0, wm->screen->width, work_height(wm) };
        window->maximized = 1;
    }
    wm_invalidate_app(wm, app);
    app_resized(wm, app);
    trace_app(wm, window->maximized ? "maximize:" : "restore:", app);
}

void wm_show_desktop(struct wm *wm)
{
    for (int app = 0; app < APP_COUNT; app++)
        wm_minimize(wm, app);
    wm_focus_desktop(wm);
}

static void move_window(struct wm *wm, int app, int x, int y)
{
    wm_invalidate_app(wm, app);
    wm->windows[app].frame.x = x;
    wm->windows[app].frame.y = y;
    clamp_frame(wm, &wm->windows[app].frame);
    wm_invalidate_app(wm, app);
}

static void resize_window(struct wm *wm, int app, int width, int height)
{
    struct rect *frame = &wm->windows[app].frame;
    int min_w, min_h;
    app_min_size(app, &min_w, &min_h);

    int max_w = wm->screen->width - frame->x;
    int max_h = work_height(wm) - frame->y;
    wm_invalidate_app(wm, app);
    frame->w = clamp(width, min_w, max_w < min_w ? min_w : max_w);
    frame->h = clamp(height, min_h, max_h < min_h ? min_h : max_h);
    wm_invalidate_app(wm, app);
    app_resized(wm, app);
}

static void set_menu(struct wm *wm, int open)
{
    if (wm->menu_open == open)
        return;
    wm->menu_open = open;
    wm->menu_index = 0;
    wm_invalidate(wm, menu_rect(wm));
    wm_invalidate(wm, taskbar_rect(wm));
    wm_trace(wm, open ? "menu:open" : "menu:close");
}

static void activate_menu_item(struct wm *wm, int index)
{
    set_menu(wm, 0);
    if (index < APP_COUNT)
        wm_open(wm, index);
    else if (index == APP_COUNT)
        wm_show_desktop(wm);
    else
        shell_request_exit(wm);
}

static void menu_key(struct wm *wm, int key)
{
    switch (key) {
    case KEY_UP:
    case KEY_DOWN:
        wm->menu_index = (wm->menu_index + (key == KEY_DOWN ? 1 : MENU_ITEMS - 1)) % MENU_ITEMS;
        wm_invalidate(wm, menu_rect(wm));
        break;
    case KEY_ENTER:
        activate_menu_item(wm, wm->menu_index);
        break;
    case KEY_F1:
    case KEY_ESC:
        set_menu(wm, 0);
        break;
    default:
        break;
    }
}

static void move_mode_key(struct wm *wm, int key)
{
    int app = wm_focused(wm);
    if (app < 0) {
        wm->move_mode = 0;
        return;
    }

    struct rect frame = wm->windows[app].frame;
    switch (key) {
    case KEY_UP:
        move_window(wm, app, frame.x, frame.y - MOVE_STEP);
        break;
    case KEY_DOWN:
        move_window(wm, app, frame.x, frame.y + MOVE_STEP);
        break;
    case KEY_LEFT:
        move_window(wm, app, frame.x - MOVE_STEP, frame.y);
        break;
    case KEY_RIGHT:
        move_window(wm, app, frame.x + MOVE_STEP, frame.y);
        break;
    case KEY_ENTER:
    case KEY_ESC:
    case KEY_F3:
        wm->move_mode = 0;
        wm_invalidate_app(wm, app);
        trace_frame(wm, "moved:", app);
        break;
    default:
        break;
    }
}

void wm_key(struct wm *wm, int key)
{
    if (wm->dialog.kind != DIALOG_NONE) {
        shell_dialog_key(wm, key);
        return;
    }
    if (wm->context.open) {
        shell_context_key(wm, key);
        return;
    }
    if (wm->menu_open) {
        menu_key(wm, key);
        return;
    }
    if (wm->move_mode) {
        move_mode_key(wm, key);
        return;
    }

    int focused = wm_focused(wm);
    switch (key) {
    case KEY_F1:
        set_menu(wm, 1);
        break;
    case KEY_TAB:
        if (wm->z_count > 1 || (wm->z_count == 1 && focused < 0))
            wm_focus(wm, wm->z[0]);
        break;
    case KEY_F3:
        if (focused >= 0 && !wm->windows[focused].maximized) {
            wm->move_mode = 1;
            wm_invalidate_app(wm, focused);
            wm_trace(wm, "move:start");
        }
        break;
    case KEY_F4:
        if (focused >= 0)
            wm_request_close(wm, focused);
        break;
    case KEY_F6:
        wm_show_desktop(wm);
        break;
    default:
        if (focused >= 0)
            app_key(wm, focused, key);
        else
            shell_desktop_key(wm, key);
        break;
    }
}

static void taskbar_press(struct wm *wm, int x, int y, int button)
{
    if (button != BUTTON_LEFT)
        return;
    if (rect_contains(start_rect(wm), x, y)) {
        set_menu(wm, 1);
        return;
    }

    int ordinal = 0;
    for (int app = 0; app < APP_COUNT; app++) {
        if (!wm->windows[app].open)
            continue;
        if (rect_contains(task_rect(wm, ordinal), x, y)) {
            if (wm_focused(wm) == app)
                wm_minimize(wm, app);
            else
                wm_focus(wm, app);
            return;
        }
        ordinal++;
    }
}

static void window_press(struct wm *wm, int app, int x, int y, int button, int clicks)
{
    wm_focus(wm, app);
    struct window *window = &wm->windows[app];
    struct rect f = window->frame;

    if (y < f.y + WM_TITLE_H) {
        if (button != BUTTON_LEFT)
            return;
        if (rect_contains(title_button(f, 0), x, y)) {
            wm_request_close(wm, app);
        } else if (app_resizable(app) && rect_contains(title_button(f, 1), x, y)) {
            wm_toggle_maximize(wm, app);
        } else if (rect_contains(title_button(f, minimize_slot(app)), x, y)) {
            wm_minimize(wm, app);
        } else if (clicks == 2 && app_resizable(app)) {
            wm_toggle_maximize(wm, app);
        } else if (!window->maximized) {
            wm->drag_app = app;
            wm->drag_resize = 0;
            wm->drag_dx = x - f.x;
            wm->drag_dy = y - f.y;
            trace_app(wm, "drag:", app);
        }
        return;
    }

    if (button == BUTTON_LEFT && app_resizable(app) && !window->maximized &&
        rect_contains(grip_rect(f), x, y)) {
        wm->drag_app = app;
        wm->drag_resize = 1;
        wm->drag_dx = f.x + f.w - x;
        wm->drag_dy = f.y + f.h - y;
        trace_app(wm, "resize:", app);
        return;
    }
    app_press(wm, app, x, y, button, clicks);
}

static int count_clicks(struct wm *wm, int x, int y)
{
    int dx = x > wm->click_x ? x - wm->click_x : wm->click_x - x;
    int dy = y > wm->click_y ? y - wm->click_y : wm->click_y - y;
    int is_double = wm->click_armed && wm->ticks - wm->last_click <= DOUBLE_CLICK_TICKS &&
                    dx <= DOUBLE_CLICK_DISTANCE && dy <= DOUBLE_CLICK_DISTANCE;

    /* A third quick click starts a new sequence. */
    wm->click_armed = !is_double;
    wm->last_click = wm->ticks;
    wm->click_x = x;
    wm->click_y = y;
    return is_double ? 2 : 1;
}

static void handle_press(struct wm *wm, int button)
{
    int x = wm->pointer_x;
    int y = wm->pointer_y;
    int clicks = button == BUTTON_LEFT ? count_clicks(wm, x, y) : 1;

    if (wm->dialog.kind != DIALOG_NONE) {
        if (button == BUTTON_LEFT)
            shell_dialog_press(wm, x, y);
        return;
    }
    if (wm->context.open && shell_context_press(wm, x, y))
        return;

    if (wm->menu_open) {
        for (int i = 0; i < MENU_ITEMS; i++) {
            if (rect_contains(menu_item_rect(wm, i), x, y)) {
                activate_menu_item(wm, i);
                return;
            }
        }
        int on_start = rect_contains(start_rect(wm), x, y);
        set_menu(wm, 0);
        if (on_start)
            return; /* The button toggles the menu closed. */
    }

    if (y >= work_height(wm)) {
        taskbar_press(wm, x, y, button);
        return;
    }

    for (int i = wm->z_count - 1; i >= 0; i--) {
        int app = wm->z[i];
        if (!wm->windows[app].minimized && rect_contains(wm->windows[app].frame, x, y)) {
            window_press(wm, app, x, y, button, clicks);
            return;
        }
    }
    shell_desktop_press(wm, x, y, button, clicks);
}

/* Returns the visible window under a point, or -1. */
static int window_at(const struct wm *wm, int x, int y)
{
    if (wm->dialog.kind != DIALOG_NONE || y >= work_height(wm))
        return -1;
    for (int i = wm->z_count - 1; i >= 0; i--) {
        int app = wm->z[i];
        if (!wm->windows[app].minimized && rect_contains(wm->windows[app].frame, x, y))
            return app;
    }
    return -1;
}

void wm_wheel(struct wm *wm, int notches)
{
    int app = window_at(wm, wm->pointer_x, wm->pointer_y);
    if (app < 0 && wm->pointer_visible == 0)
        app = wm_focused(wm);
    if (app >= 0 && notches != 0 && !wm->menu_open && !wm->context.open)
        app_wheel(wm, app, notches);
}

void wm_pointer(struct wm *wm, int dx, int dy, int buttons)
{
    if (dx != 0 || dy != 0 || !wm->pointer_visible) {
        if (wm->pointer_visible)
            wm_invalidate(wm, pointer_rect(wm));
        wm->pointer_x = clamp(wm->pointer_x + dx, 0, wm->screen->width - 1);
        wm->pointer_y = clamp(wm->pointer_y + dy, 0, wm->screen->height - 1);
        wm->pointer_visible = 1;
        wm_invalidate(wm, pointer_rect(wm));

        if (wm->menu_open) {
            for (int i = 0; i < MENU_ITEMS; i++) {
                if (i != wm->menu_index &&
                    rect_contains(menu_item_rect(wm, i), wm->pointer_x, wm->pointer_y)) {
                    wm->menu_index = i;
                    wm_invalidate(wm, menu_rect(wm));
                }
            }
        }
        if (wm->context.open)
            shell_context_hover(wm, wm->pointer_x, wm->pointer_y);
        if (wm->drag_app < 0) {
            int app = window_at(wm, wm->pointer_x, wm->pointer_y);
            if (app >= 0)
                app_hover(wm, app, wm->pointer_x, wm->pointer_y);
        }

        if (wm->drag_app >= 0) {
            const struct rect f = wm->windows[wm->drag_app].frame;
            if (wm->drag_resize)
                resize_window(wm, wm->drag_app, wm->pointer_x + wm->drag_dx - f.x,
                              wm->pointer_y + wm->drag_dy - f.y);
            else
                move_window(wm, wm->drag_app, wm->pointer_x - wm->drag_dx,
                            wm->pointer_y - wm->drag_dy);
        }
    }

    int pressed = buttons & ~wm->buttons;
    int released = wm->buttons & ~buttons;
    wm->buttons = buttons;

    if (pressed & BUTTON_LEFT)
        handle_press(wm, BUTTON_LEFT);
    if (pressed & BUTTON_RIGHT)
        handle_press(wm, BUTTON_RIGHT);
    if ((released & BUTTON_LEFT) && wm->drag_app >= 0) {
        trace_frame(wm, "moved:", wm->drag_app);
        wm->drag_app = -1;
    }
}

void wm_tick(struct wm *wm)
{
    wm->ticks++;
    if (wm->net) {
        browser_tick(wm);
        terminal_tick(wm);
    }
}

void wm_set_clock(struct wm *wm, const char *clock)
{
    if (text_equal(wm->clock, clock))
        return;
    text_copy(wm->clock, sizeof wm->clock, clock);
    wm_invalidate(wm, taskbar_rect(wm));
}

static void draw_title_button(struct canvas *c, struct rect button, int kind, int focused)
{
    uint32_t color = focused ? UI_ACCENT : UI_DIM;
    canvas_frame(c, button, focused ? UI_ACCENT : UI_BORDER);
    if (kind == 0)
        canvas_text(c, button.x + 3, button.y + 2, "x", color);
    else if (kind == 1)
        canvas_frame(c, (struct rect){ button.x + 4, button.y + 4, 10, 10 }, color);
    else
        canvas_fill(c, (struct rect){ button.x + 4, button.y + 12, 10, 2 }, color);
}

static void draw_window(struct wm *wm, int app, int focused)
{
    struct canvas *c = wm->screen;
    struct window *window = &wm->windows[app];
    struct rect f = window->frame;

    if (!window->maximized)
        canvas_blend(c, (struct rect){ f.x + SHADOW, f.y + SHADOW, f.w, f.h }, 0, 110);
    canvas_fill(c, f, UI_WINDOW);
    canvas_fill(c, (struct rect){ f.x, f.y, f.w, WM_TITLE_H }, focused ? UI_TITLE_FOCUS : UI_TITLE);
    canvas_frame(c, f, focused ? UI_ACCENT : UI_BORDER);

    char title[48];
    app_window_title(wm, app, title, sizeof title);
    if (focused && wm->move_mode)
        text_append(title, sizeof title, " [move]");
    int title_columns = (f.w - 18 - (minimize_slot(app) + 1) * BUTTON_STEP) / FONT_ADVANCE;
    ui_text_fit(c, f.x + 10, f.y + 7, title, title_columns, focused ? UI_TEXT : UI_DIM);

    draw_title_button(c, title_button(f, 0), 0, focused);
    if (app_resizable(app))
        draw_title_button(c, title_button(f, 1), 1, focused);
    draw_title_button(c, title_button(f, minimize_slot(app)), 2, focused);

    struct rect saved = c->clip;
    struct rect content = wm_content_rect(wm, app);
    struct rect visible;
    if (rect_intersect(content, saved, &visible)) {
        c->clip = visible;
        app_draw(wm, app, content, focused);
        c->clip = saved;
    }

    if (app_resizable(app) && !window->maximized) {
        struct rect grip = grip_rect(f);
        for (int i = 0; i < 3; i++) {
            canvas_fill(c, (struct rect){ grip.x + 10 - i * 4, grip.y + 10, 2, 2 }, UI_DIM);
            if (i < 2)
                canvas_fill(c, (struct rect){ grip.x + 10 - i * 4, grip.y + 6, 2, 2 }, UI_DIM);
        }
        canvas_fill(c, (struct rect){ grip.x + 10, grip.y + 2, 2, 2 }, UI_DIM);
    }
}

static void draw_cat_icon(struct canvas *c, int x, int y)
{
    for (int row = 0; row < 6; row++) {
        for (int col = 0; col < 7; col++) {
            if (CAT_ICON[row][col] == '#')
                canvas_fill(c, (struct rect){ x + col * 2, y + row * 2, 2, 2 }, UI_ACCENT);
        }
    }
}

static void draw_taskbar(struct wm *wm)
{
    struct canvas *c = wm->screen;
    struct rect bar = taskbar_rect(wm);
    canvas_blend(c, bar, UI_PANEL, 236);
    canvas_fill(c, (struct rect){ bar.x, bar.y, bar.w, 1 }, UI_BORDER);

    struct rect start = start_rect(wm);
    canvas_fill(c, start, wm->menu_open ? UI_TITLE_FOCUS : UI_TITLE);
    draw_cat_icon(c, start.x + 7, start.y + 8);
    canvas_text(c, start.x + 30, start.y + 7, "CATO", UI_TEXT);

    int focused = wm_focused(wm);
    int ordinal = 0;
    for (int app = 0; app < APP_COUNT; app++) {
        if (!wm->windows[app].open)
            continue;
        struct rect task = task_rect(wm, ordinal++);
        int minimized = wm->windows[app].minimized;
        canvas_fill(c, task, app == focused ? UI_TITLE_FOCUS : UI_TITLE);
        if (app == focused)
            canvas_fill(c, (struct rect){ task.x, task.y + task.h - 2, task.w, 2 }, UI_ACCENT);
        else if (!minimized)
            canvas_fill(c, (struct rect){ task.x, task.y + task.h - 2, task.w, 2 }, UI_DIM);
        ui_text_fit(c, task.x + 8, task.y + 7, app_title(app), (task.w - 12) / FONT_ADVANCE,
                    app == focused ? UI_TEXT : UI_DIM);
    }

    canvas_text(c, c->width - FONT_ADVANCE * 16 - 12, bar.y + 11, wm->clock, UI_TEXT);
}

static void draw_menu(struct wm *wm)
{
    struct canvas *c = wm->screen;
    struct rect panel = menu_rect(wm);
    canvas_blend(c, (struct rect){ panel.x + SHADOW, panel.y + SHADOW, panel.w, panel.h }, 0, 110);
    canvas_fill(c, panel, UI_WINDOW);
    canvas_frame(c, panel, UI_ACCENT);
    for (int i = 0; i < MENU_ITEMS; i++) {
        struct rect item = menu_item_rect(wm, i);
        if (i == wm->menu_index)
            canvas_fill(c, item, UI_TITLE_FOCUS);
        canvas_text(c, item.x + 8, item.y + 8, MENU_LABELS[i],
                    i == wm->menu_index ? UI_ACCENT : UI_TEXT);
    }
}

static void draw_pointer(struct wm *wm)
{
    for (int row = 0; row < POINTER_H; row++) {
        for (int col = 0; col < POINTER_W; col++) {
            char cell = POINTER_SHAPE[row][col];
            if (cell == ' ')
                continue;
            canvas_fill(wm->screen,
                        (struct rect){ wm->pointer_x + col, wm->pointer_y + row, 1, 1 },
                        cell == '#' ? RGB(0, 0, 0) : RGB(255, 255, 255));
        }
    }
}

void wm_render(struct wm *wm)
{
    struct canvas *c = wm->screen;
    int focused = wm_focused(wm);

    for (int i = 0; i < wm->dirty_count; i++) {
        canvas_set_clip(c, wm->dirty[i]);
        canvas_copy(c, wm->dirty[i], wm->wallpaper);
        shell_draw_desktop(wm);
        for (int z = 0; z < wm->z_count; z++) {
            if (!wm->windows[wm->z[z]].minimized)
                draw_window(wm, wm->z[z], wm->z[z] == focused);
        }
        draw_taskbar(wm);
        if (wm->menu_open)
            draw_menu(wm);
        shell_draw_overlays(wm);
        if (wm->pointer_visible)
            draw_pointer(wm);
    }
    canvas_set_clip(c, (struct rect){ 0, 0, c->width, c->height });
}
