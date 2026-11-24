/* App registry and the System information app. */
#include "apps.h"

#include "net.h"
#include "text.h"

#define PADDING 8
#define SYSTEM_COLS 46
#define SYSTEM_ROWS 9
#define SYSTEM_ROW_H 24
#define SYSTEM_LABEL_COLS 10

const char *app_title(int app)
{
    switch (app) {
    case APP_FILES:
        return "Files";
    case APP_EDITOR:
        return "Editor";
    case APP_TERMINAL:
        return "Terminal";
    case APP_BROWSER:
        return "Browser";
    default:
        return "System";
    }
}

const char *app_slug(int app)
{
    switch (app) {
    case APP_FILES:
        return "files";
    case APP_EDITOR:
        return "editor";
    case APP_TERMINAL:
        return "terminal";
    case APP_BROWSER:
        return "browser";
    default:
        return "system";
    }
}

void app_window_title(const struct wm *wm, int app, char *out, int capacity)
{
    if (app == APP_EDITOR) {
        text_copy(out, capacity, wm->editor.name);
        if (wm->editor.dirty)
            text_append(out, capacity, " *");
    } else if (app == APP_FILES) {
        text_copy(out, capacity, "Files - ");
        char path[40];
        fs_path(wm->fs, wm->files.cwd, path, sizeof path);
        text_append(out, capacity, path);
    } else if (app == APP_BROWSER && browser_page_title(wm)[0]) {
        text_copy(out, capacity, browser_page_title(wm));
        text_append(out, capacity, " - Browser");
    } else {
        text_copy(out, capacity, app_title(app));
    }
}

void app_default_size(int app, int *width, int *height)
{
    switch (app) {
    case APP_FILES:
        *width = 860;
        *height = 520;
        break;
    case APP_EDITOR:
        *width = 720;
        *height = 460;
        break;
    case APP_TERMINAL:
        *width = TERM_COLS * FONT_ADVANCE + PADDING * 2 + 2;
        *height = WM_TITLE_H + TERM_ROWS * FONT_LINE + PADDING * 2 + 1;
        break;
    case APP_BROWSER:
        *width = 980;
        *height = 620;
        break;
    default:
        *width = SYSTEM_COLS * FONT_ADVANCE + PADDING * 2 + 2;
        *height = WM_TITLE_H + SYSTEM_ROWS * SYSTEM_ROW_H + PADDING * 2 + 1;
        break;
    }
}

void app_min_size(int app, int *width, int *height)
{
    if (app == APP_FILES) {
        *width = 720;
        *height = 320;
    } else if (app == APP_EDITOR) {
        *width = 360;
        *height = 220;
    } else if (app == APP_BROWSER) {
        *width = 640;
        *height = 360;
    } else {
        app_default_size(app, width, height);
    }
}

int app_resizable(int app)
{
    return app == APP_FILES || app == APP_EDITOR || app == APP_BROWSER;
}

void app_resized(struct wm *wm, int app)
{
    if (app == APP_FILES)
        files_refresh(wm);
    else if (app == APP_EDITOR)
        editor_ensure_visible(wm);
    else if (app == APP_BROWSER)
        browser_resized(wm);
}

static void draw_system(struct wm *wm, struct rect content)
{
    const struct sysinfo *info = wm->info;
    char storage[36];
    storage[0] = 0;
    text_append_u64(storage, sizeof storage, fs_used_bytes(wm->fs) / 1024);
    text_append(storage, sizeof storage, " KiB of ");
    text_append_u64(storage, sizeof storage, (FS_BLOCKS * FS_BLOCK_SIZE) / 1024);
    text_append(storage, sizeof storage, wm->storage_ready ? " KiB (USB)" : " KiB (RAM only)");

    char network[36] = "Unavailable";
    if (wm->net) {
        text_copy(network, sizeof network, net_state_text(wm->net));
        if (wm->net->state == NET_READY) {
            char address[20];
            net_format_ip(wm->net->ip, address, sizeof address);
            text_append(network, sizeof network, ", ");
            text_append(network, sizeof network, address);
        }
    }

    const char *rows[SYSTEM_ROWS][2] = {
        { "OS", "Cato OS desktop preview" },
        { "Firmware", info->firmware },
        { "UEFI", info->uefi },
        { "CPU", info->cpu },
        { "Memory", info->memory },
        { "Files", storage },
        { "Network", network },
        { "Display", info->display },
        { "Pointer", info->pointer },
    };

    int x = content.x + PADDING;
    int y = content.y + PADDING + 4;
    for (int i = 0; i < SYSTEM_ROWS; i++, y += SYSTEM_ROW_H) {
        char value[SYSTEM_COLS - SYSTEM_LABEL_COLS + 1];
        text_copy(value, sizeof value, rows[i][1]);
        canvas_text(wm->screen, x, y, rows[i][0], UI_ACCENT);
        canvas_text(wm->screen, x + SYSTEM_LABEL_COLS * FONT_ADVANCE, y, value, UI_TEXT);
    }
}

void app_draw(struct wm *wm, int app, struct rect content, int focused)
{
    switch (app) {
    case APP_FILES:
        files_draw(wm, content, focused);
        break;
    case APP_EDITOR:
        editor_draw(wm, content, focused);
        break;
    case APP_TERMINAL:
        terminal_draw(wm, content, focused);
        break;
    case APP_BROWSER:
        browser_draw(wm, content, focused);
        break;
    default:
        draw_system(wm, content);
        break;
    }
}

void app_key(struct wm *wm, int app, int key)
{
    if (app == APP_FILES)
        files_key(wm, key);
    else if (app == APP_EDITOR)
        editor_key(wm, key);
    else if (app == APP_TERMINAL)
        terminal_key(wm, key);
    else if (app == APP_BROWSER)
        browser_key(wm, key);
}

void app_hover(struct wm *wm, int app, int x, int y)
{
    if (app == APP_BROWSER)
        browser_hover(wm, x, y);
}

void app_wheel(struct wm *wm, int app, int notches)
{
    if (app == APP_BROWSER) {
        browser_wheel(wm, notches);
    } else if (app == APP_FILES || app == APP_EDITOR) {
        int key = notches > 0 ? KEY_DOWN : KEY_UP;
        int steps = (notches > 0 ? notches : -notches) * 3;
        for (int i = 0; i < steps; i++)
            app_key(wm, app, key);
    }
}

void app_press(struct wm *wm, int app, int x, int y, int button, int clicks)
{
    if (app == APP_FILES)
        files_press(wm, x, y, button, clicks);
    else if (app == APP_EDITOR)
        editor_press(wm, x, y, button);
    else if (app == APP_BROWSER)
        browser_press(wm, x, y, button);
}
