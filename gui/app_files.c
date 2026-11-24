/* The Files app: browse folders and manage files. */
#include "apps.h"
#include "icons.h"
#include "shell.h"
#include "text.h"

#define TOOLBAR_H 34
#define PATH_H 26
#define SIDEBAR_W 156
#define HEADER_H 22
#define ROW_H 24
#define STATUS_H 24
#define SCROLLBAR_W 10
#define DATE_W 204
#define SIZE_W 96
#define PLACES 6

enum tool {
    TOOL_BACK,
    TOOL_UP,
    TOOL_NEW_FOLDER,
    TOOL_NEW_FILE,
    TOOL_RENAME,
    TOOL_COPY,
    TOOL_CUT,
    TOOL_PASTE,
    TOOL_DELETE,
    TOOL_COUNT,
};

static const char *const TOOL_LABELS[TOOL_COUNT] = {
    "Back", "Up", "+Folder", "+File", "Rename", "Copy", "Cut", "Paste", "Delete",
};

static const char *const PLACE_LABELS[PLACES] = {
    "Home", "Desktop", "Documents", "Downloads", "Pictures", "Trash",
};

struct files_layout {
    struct rect toolbar;
    struct rect path;
    struct rect sidebar;
    struct rect header;
    struct rect list;
    struct rect scrollbar;
    struct rect status;
    int rows;
};

static struct files_layout compute_layout(struct rect c)
{
    struct files_layout l;
    int body_y = c.y + TOOLBAR_H + PATH_H;
    int body_h = c.h - TOOLBAR_H - PATH_H - STATUS_H;

    l.toolbar = (struct rect){ c.x, c.y, c.w, TOOLBAR_H };
    l.path = (struct rect){ c.x, c.y + TOOLBAR_H, c.w, PATH_H };
    l.sidebar = (struct rect){ c.x, body_y, SIDEBAR_W, body_h };
    l.header = (struct rect){ c.x + SIDEBAR_W, body_y, c.w - SIDEBAR_W, HEADER_H };
    l.list = (struct rect){ c.x + SIDEBAR_W, body_y + HEADER_H, c.w - SIDEBAR_W - SCROLLBAR_W,
                            body_h - HEADER_H };
    l.scrollbar = (struct rect){ l.list.x + l.list.w, l.list.y, SCROLLBAR_W, l.list.h };
    l.status = (struct rect){ c.x, c.y + c.h - STATUS_H, c.w, STATUS_H };
    l.rows = l.list.h / ROW_H;
    if (l.rows < 1)
        l.rows = 1;
    return l;
}

static struct files_layout current_layout(const struct wm *wm)
{
    return compute_layout(wm_content_rect(wm, APP_FILES));
}

static struct rect tool_rect(struct rect toolbar, int tool)
{
    int x = toolbar.x + 6;
    for (int i = 0; i < tool; i++)
        x += text_len(TOOL_LABELS[i]) * FONT_ADVANCE + 12 + 4;
    return (struct rect){ x, toolbar.y + 5, text_len(TOOL_LABELS[tool]) * FONT_ADVANCE + 12, 24 };
}

static struct rect place_rect(struct rect sidebar, int index)
{
    return (struct rect){ sidebar.x + 4, sidebar.y + 6 + index * 28, sidebar.w - 8, 26 };
}

static int place_dir(const struct wm *wm, int index)
{
    const struct fs *fs = wm->fs;
    const int dirs[PLACES] = { FS_ROOT,       fs->desktop,  fs->documents,
                               fs->downloads, fs->pictures, fs->trash };
    return dirs[index];
}

static void clamp_scroll(struct wm *wm)
{
    struct files_app *f = &wm->files;
    int rows = wm->windows[APP_FILES].open ? current_layout(wm).rows : 1;
    int max = f->count - rows;
    if (f->scroll > max)
        f->scroll = max;
    if (f->scroll < 0)
        f->scroll = 0;
}

static void reveal_selection(struct wm *wm)
{
    struct files_app *f = &wm->files;
    if (f->selected < 0 || !wm->windows[APP_FILES].open)
        return;
    int rows = current_layout(wm).rows;
    if (f->selected < f->scroll)
        f->scroll = f->selected;
    if (f->selected >= f->scroll + rows)
        f->scroll = f->selected - rows + 1;
}

void files_init(struct wm *wm)
{
    struct files_app *f = &wm->files;
    f->cwd = FS_ROOT;
    f->count = 0;
    f->selected = -1;
    f->scroll = 0;
    f->history_len = 0;
    files_refresh(wm);
}

void files_refresh(struct wm *wm)
{
    struct files_app *f = &wm->files;
    int selected_node = f->selected >= 0 && f->selected < f->count ? f->entries[f->selected]
                                                                   : FS_NONE;
    f->count = fs_list(wm->fs, f->cwd, f->entries, FS_MAX_NODES);
    f->selected = -1;
    for (int i = 0; i < f->count; i++) {
        if (f->entries[i] == selected_node)
            f->selected = i;
    }
    clamp_scroll(wm);
    wm_invalidate_app(wm, APP_FILES);
}

static void change_dir(struct wm *wm, int dir)
{
    struct files_app *f = &wm->files;
    f->cwd = dir;
    f->selected = -1;
    f->scroll = 0;
    files_refresh(wm);
}

void files_open_dir(struct wm *wm, int dir)
{
    struct files_app *f = &wm->files;
    if (dir == f->cwd || !fs_is_dir(wm->fs, dir))
        return;

    if (f->history_len == FILES_HISTORY) {
        for (int i = 1; i < FILES_HISTORY; i++)
            f->history[i - 1] = f->history[i];
        f->history_len--;
    }
    f->history[f->history_len++] = f->cwd;
    change_dir(wm, dir);
}

void files_select_node(struct wm *wm, int node)
{
    struct files_app *f = &wm->files;
    for (int i = 0; i < f->count; i++) {
        if (f->entries[i] == node) {
            f->selected = i;
            reveal_selection(wm);
            wm_invalidate_app(wm, APP_FILES);
            return;
        }
    }
}

int files_selected(const struct wm *wm)
{
    const struct files_app *f = &wm->files;
    return f->selected >= 0 && f->selected < f->count ? f->entries[f->selected] : FS_NONE;
}

void files_forget(struct wm *wm, int node)
{
    struct files_app *f = &wm->files;
    const struct fs *fs = wm->fs;

    int kept = 0;
    for (int i = 0; i < f->history_len; i++) {
        if (!fs_is_within(fs, f->history[i], node))
            f->history[kept++] = f->history[i];
    }
    f->history_len = kept;

    if (fs_is_within(fs, f->cwd, node))
        change_dir(wm, fs->nodes[node].parent);
    else if (files_selected(wm) != FS_NONE && fs_is_within(fs, files_selected(wm), node))
        f->selected = -1;
}

static void go_back(struct wm *wm)
{
    struct files_app *f = &wm->files;
    if (f->history_len > 0)
        change_dir(wm, f->history[--f->history_len]);
}

static void go_up(struct wm *wm)
{
    if (wm->files.cwd != FS_ROOT)
        files_open_dir(wm, wm->fs->nodes[wm->files.cwd].parent);
}

static void select_index(struct wm *wm, int index)
{
    struct files_app *f = &wm->files;
    if (f->count == 0)
        return;
    if (index < 0)
        index = 0;
    if (index >= f->count)
        index = f->count - 1;
    f->selected = index;
    reveal_selection(wm);
    wm_invalidate_app(wm, APP_FILES);
}

static int tool_enabled(const struct wm *wm, int tool)
{
    const struct fs *fs = wm->fs;
    int node = files_selected(wm);
    int in_trash = fs_is_within(fs, wm->files.cwd, fs->trash);
    int editable = node != FS_NONE && !fs->nodes[node].is_protected && !in_trash;

    switch (tool) {
    case TOOL_BACK:
        return wm->files.history_len > 0;
    case TOOL_UP:
        return wm->files.cwd != FS_ROOT;
    case TOOL_NEW_FOLDER:
    case TOOL_NEW_FILE:
        return !in_trash;
    case TOOL_RENAME:
    case TOOL_COPY:
    case TOOL_CUT:
        return editable;
    case TOOL_PASTE:
        return wm->clipboard.node != FS_NONE && !in_trash;
    default:
        return node != FS_NONE && !fs->nodes[node].is_protected;
    }
}

static void run_tool(struct wm *wm, int tool)
{
    int node = files_selected(wm);
    int dir = wm->files.cwd;
    if (!tool_enabled(wm, tool))
        return;

    switch (tool) {
    case TOOL_BACK:
        go_back(wm);
        break;
    case TOOL_UP:
        go_up(wm);
        break;
    case TOOL_NEW_FOLDER:
        shell_action(wm, ACTION_NEW_FOLDER, FS_NONE, dir);
        break;
    case TOOL_NEW_FILE:
        shell_action(wm, ACTION_NEW_FILE, FS_NONE, dir);
        break;
    case TOOL_RENAME:
        shell_action(wm, ACTION_RENAME, node, dir);
        break;
    case TOOL_COPY:
        shell_action(wm, ACTION_COPY, node, dir);
        break;
    case TOOL_CUT:
        shell_action(wm, ACTION_CUT, node, dir);
        break;
    case TOOL_PASTE:
        shell_action(wm, ACTION_PASTE, FS_NONE, dir);
        break;
    default:
        shell_action(wm, ACTION_TRASH, node, dir);
        break;
    }
}

void files_key(struct wm *wm, int key)
{
    struct files_app *f = &wm->files;
    int page = wm->windows[APP_FILES].open ? current_layout(wm).rows : 1;
    int node = files_selected(wm);

    switch (key) {
    case KEY_UP:
        select_index(wm, f->selected < 0 ? 0 : f->selected - 1);
        break;
    case KEY_DOWN:
        select_index(wm, f->selected + 1);
        break;
    case KEY_PGUP:
        select_index(wm, f->selected - page);
        break;
    case KEY_PGDN:
        select_index(wm, f->selected + page);
        break;
    case KEY_HOME:
        select_index(wm, 0);
        break;
    case KEY_END:
        select_index(wm, f->count - 1);
        break;
    case KEY_ENTER:
        if (node != FS_NONE)
            shell_action(wm, ACTION_OPEN, node, f->cwd);
        break;
    case KEY_BACKSPACE:
        go_up(wm);
        break;
    case KEY_F2:
        run_tool(wm, TOOL_RENAME);
        break;
    case KEY_F5:
        files_refresh(wm);
        break;
    case KEY_DELETE:
        run_tool(wm, TOOL_DELETE);
        break;
    case KEY_CTRL_C:
        run_tool(wm, TOOL_COPY);
        break;
    case KEY_CTRL_X:
        run_tool(wm, TOOL_CUT);
        break;
    case KEY_CTRL_V:
        run_tool(wm, TOOL_PASTE);
        break;
    case KEY_CTRL_N:
        run_tool(wm, TOOL_NEW_FOLDER);
        break;
    default:
        break;
    }
}

void files_press(struct wm *wm, int x, int y, int button, int clicks)
{
    struct files_app *f = &wm->files;
    struct files_layout l = current_layout(wm);

    if (button == BUTTON_LEFT && rect_contains(l.toolbar, x, y)) {
        for (int tool = 0; tool < TOOL_COUNT; tool++) {
            if (rect_contains(tool_rect(l.toolbar, tool), x, y)) {
                run_tool(wm, tool);
                return;
            }
        }
        return;
    }

    if (rect_contains(l.sidebar, x, y)) {
        for (int i = 0; i < PLACES; i++) {
            if (!rect_contains(place_rect(l.sidebar, i), x, y))
                continue;
            if (button == BUTTON_RIGHT)
                shell_context_dir(wm, x, y, place_dir(wm, i));
            else
                files_open_dir(wm, place_dir(wm, i));
            return;
        }
        return;
    }

    if (button == BUTTON_LEFT && rect_contains(l.scrollbar, x, y)) {
        int thumb_y = f->count > 0 ? l.scrollbar.y + l.scrollbar.h * f->scroll / f->count : 0;
        f->scroll += y < thumb_y ? -l.rows : l.rows;
        clamp_scroll(wm);
        wm_invalidate_app(wm, APP_FILES);
        return;
    }

    if (!rect_contains(l.list, x, y))
        return;

    int index = f->scroll + (y - l.list.y) / ROW_H;
    if (index >= f->count) {
        f->selected = -1;
        wm_invalidate_app(wm, APP_FILES);
        if (button == BUTTON_RIGHT)
            shell_context_dir(wm, x, y, f->cwd);
        return;
    }

    select_index(wm, index);
    if (button == BUTTON_RIGHT)
        shell_context_node(wm, x, y, f->entries[index]);
    else if (clicks == 2)
        shell_action(wm, ACTION_OPEN, f->entries[index], f->cwd);
}

static void format_size(const struct fs *fs, int node, char *out, int capacity)
{
    out[0] = 0;
    if (fs_is_dir(fs, node)) {
        int items[FS_MAX_NODES];
        int count = fs_list(fs, node, items, FS_MAX_NODES);
        text_append_u64(out, capacity, (uint64_t)count);
        text_append(out, capacity, count == 1 ? " item" : " items");
        return;
    }
    uint32_t size = fs->nodes[node].size;
    if (size < 1024) {
        text_append_u64(out, capacity, size);
        text_append(out, capacity, " B");
    } else {
        text_append_u64(out, capacity, (size + 512) / 1024);
        text_append(out, capacity, " KB");
    }
}

static void draw_toolbar(struct wm *wm, const struct files_layout *l)
{
    struct canvas *c = wm->screen;
    canvas_fill(c, l->toolbar, UI_PANEL);
    for (int tool = 0; tool < TOOL_COUNT; tool++) {
        struct rect r = tool_rect(l->toolbar, tool);
        int enabled = tool_enabled(wm, tool);
        canvas_fill(c, r, enabled ? UI_TITLE : UI_PANEL);
        canvas_frame(c, r, UI_BORDER);
        canvas_text(c, r.x + 6, r.y + 5, TOOL_LABELS[tool], enabled ? UI_TEXT : UI_BORDER);
    }
}

static void draw_sidebar(struct wm *wm, const struct files_layout *l)
{
    struct canvas *c = wm->screen;
    canvas_fill(c, l->sidebar, UI_PANEL);
    canvas_fill(c, (struct rect){ l->sidebar.x + l->sidebar.w - 1, l->sidebar.y, 1, l->sidebar.h },
                UI_BORDER);

    for (int i = 0; i < PLACES; i++) {
        struct rect r = place_rect(l->sidebar, i);
        int active = place_dir(wm, i) == wm->files.cwd;
        if (active)
            canvas_fill(c, r, UI_TITLE_FOCUS);
        icon_draw(c, i == PLACES - 1 ? ICON_TRASH : ICON_FOLDER, r.x + 4, r.y + 5, 1);
        ui_text_fit(c, r.x + 24, r.y + 6, PLACE_LABELS[i], (r.w - 26) / FONT_ADVANCE,
                    active ? UI_ACCENT : UI_TEXT);
    }
}

static void draw_list(struct wm *wm, const struct files_layout *l, int focused)
{
    struct canvas *c = wm->screen;
    const struct files_app *f = &wm->files;
    const struct fs *fs = wm->fs;
    int name_w = l->list.w - DATE_W - SIZE_W;

    canvas_fill(c, l->header, UI_TITLE);
    canvas_text(c, l->header.x + 8, l->header.y + 4, "Name", UI_DIM);
    canvas_text(c, l->header.x + name_w, l->header.y + 4, "Modified", UI_DIM);
    canvas_text(c, l->header.x + name_w + DATE_W, l->header.y + 4, "Size", UI_DIM);

    for (int row = 0; row < l->rows && f->scroll + row < f->count; row++) {
        int index = f->scroll + row;
        int node = f->entries[index];
        const struct fs_node *n = &fs->nodes[node];
        struct rect r = { l->list.x, l->list.y + row * ROW_H, l->list.w, ROW_H };

        if (index == f->selected) {
            canvas_fill(c, r, focused ? UI_TITLE_FOCUS : UI_TITLE);
            canvas_frame(c, r, focused ? UI_ACCENT : UI_BORDER);
        }

        int is_cut = wm->clipboard.cut && wm->clipboard.node == node;
        uint32_t color = is_cut ? UI_DIM : UI_TEXT;
        int icon = node == fs->trash ? ICON_TRASH : n->type == FS_DIR ? ICON_FOLDER : ICON_FILE;
        icon_draw(c, icon, r.x + 6, r.y + 4, 1);
        ui_text_fit(c, r.x + 28, r.y + 5, n->name, (name_w - 32) / FONT_ADVANCE, color);

        char when[17];
        char size[16];
        fs_format_time(n->modified, when, sizeof when);
        format_size(fs, node, size, sizeof size);
        canvas_text(c, r.x + name_w, r.y + 5, when, UI_DIM);
        ui_text_fit(c, r.x + name_w + DATE_W, r.y + 5, size, (SIZE_W - 4) / FONT_ADVANCE, UI_DIM);
    }

    if (f->count == 0) {
        const char *empty = f->cwd == fs->trash ? "Trash is empty" : "This folder is empty";
        canvas_text(c, l->list.x + 16, l->list.y + 16, empty, UI_DIM);
    }

    canvas_fill(c, l->scrollbar, UI_PANEL);
    if (f->count > l->rows) {
        int thumb_h = l->scrollbar.h * l->rows / f->count;
        if (thumb_h < 16)
            thumb_h = 16;
        int thumb_y = l->scrollbar.y + (l->scrollbar.h - thumb_h) * f->scroll / (f->count - l->rows);
        canvas_fill(c, (struct rect){ l->scrollbar.x + 2, thumb_y, l->scrollbar.w - 4, thumb_h },
                    UI_DIM);
    }
}

void files_draw(struct wm *wm, struct rect content, int focused)
{
    struct canvas *c = wm->screen;
    const struct files_app *f = &wm->files;
    const struct fs *fs = wm->fs;
    struct files_layout l = compute_layout(content);

    draw_toolbar(wm, &l);

    char path[128];
    fs_path(fs, f->cwd, path, sizeof path);
    canvas_fill(c, l.path, UI_WINDOW);
    canvas_fill(c, (struct rect){ l.path.x, l.path.y + l.path.h - 1, l.path.w, 1 }, UI_BORDER);
    ui_text_fit(c, l.path.x + 8, l.path.y + 6, path, (l.path.w - 16) / FONT_ADVANCE, UI_TEXT);

    draw_sidebar(wm, &l);
    draw_list(wm, &l, focused);

    char status[64];
    status[0] = 0;
    text_append_u64(status, sizeof status, (uint64_t)f->count);
    text_append(status, sizeof status, f->count == 1 ? " item   " : " items   ");
    text_append_u64(status, sizeof status, fs_used_bytes(fs) / 1024);
    text_append(status, sizeof status,
                wm->storage_ready ? " KiB used (saved to USB)" : " KiB used (RAM only)");
    canvas_fill(c, l.status, UI_PANEL);
    canvas_fill(c, (struct rect){ l.status.x, l.status.y, l.status.w, 1 }, UI_BORDER);
    ui_text_fit(c, l.status.x + 8, l.status.y + 6, status, (l.status.w - 32) / FONT_ADVANCE, UI_DIM);
}
