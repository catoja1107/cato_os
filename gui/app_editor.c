/* The Text Editor app. */
#include "apps.h"
#include "shell.h"
#include "text.h"

#define TOOLBAR_H 34
#define STATUS_H 24
#define PADDING 8
#define MAX_LINE_DRAW 255
#define DEFAULT_NAME "Untitled.txt"

static struct rect save_button(struct rect c)
{
    return (struct rect){ c.x + 6, c.y + 5, 4 * FONT_ADVANCE + 12, 24 };
}

static struct rect save_as_button(struct rect c)
{
    struct rect save = save_button(c);
    return (struct rect){ save.x + save.w + 4, c.y + 5, 7 * FONT_ADVANCE + 12, 24 };
}

static struct rect text_area(struct rect c)
{
    return (struct rect){ c.x + PADDING, c.y + TOOLBAR_H + PADDING, c.w - PADDING * 2,
                          c.h - TOOLBAR_H - STATUS_H - PADDING * 2 };
}

static int visible_rows(struct rect c)
{
    int rows = text_area(c).h / FONT_LINE;
    return rows < 1 ? 1 : rows;
}

static int visible_cols(struct rect c)
{
    int cols = text_area(c).w / FONT_ADVANCE;
    if (cols > MAX_LINE_DRAW)
        cols = MAX_LINE_DRAW;
    return cols < 1 ? 1 : cols;
}

static int line_start(const struct editor_app *e, int position)
{
    while (position > 0 && e->text[position - 1] != '\n')
        position--;
    return position;
}

static int line_end(const struct editor_app *e, int position)
{
    while (position < e->len && e->text[position] != '\n')
        position++;
    return position;
}

static int line_number(const struct editor_app *e, int position)
{
    int line = 0;
    for (int i = 0; i < position; i++) {
        if (e->text[i] == '\n')
            line++;
    }
    return line;
}

/* Returns the start of a line, or of the last line if the document is shorter. */
static int line_offset(const struct editor_app *e, int line)
{
    int position = 0;
    while (line > 0) {
        int end = line_end(e, position);
        if (end >= e->len)
            break;
        position = end + 1;
        line--;
    }
    return position;
}

void editor_ensure_visible(struct wm *wm)
{
    struct editor_app *e = &wm->editor;
    if (!wm->windows[APP_EDITOR].open)
        return;

    struct rect content = wm_content_rect(wm, APP_EDITOR);
    int rows = visible_rows(content);
    int cols = visible_cols(content);
    int line = line_number(e, e->cursor);
    int column = e->cursor - line_start(e, e->cursor);

    if (line < e->top_line)
        e->top_line = line;
    if (line >= e->top_line + rows)
        e->top_line = line - rows + 1;
    if (column < e->left_col)
        e->left_col = column;
    if (column >= e->left_col + cols)
        e->left_col = column - cols + 1;
}

static void changed_view(struct wm *wm)
{
    editor_ensure_visible(wm);
    wm_invalidate_app(wm, APP_EDITOR);
}

static void load(struct wm *wm, int node)
{
    struct editor_app *e = &wm->editor;
    int length = fs_read(wm->fs, node, 0, e->text, EDITOR_MAX - 1);

    e->len = length > 0 ? length : 0;
    e->text[e->len] = 0;
    e->node = node;
    e->dir = wm->fs->nodes[node].parent;
    text_copy(e->name, sizeof e->name, wm->fs->nodes[node].name);
    e->cursor = 0;
    e->top_line = 0;
    e->left_col = 0;
    e->dirty = 0;
}

void editor_reset(struct wm *wm)
{
    struct editor_app *e = &wm->editor;
    e->node = FS_NONE;
    e->dir = FS_NONE;
    text_copy(e->name, sizeof e->name, DEFAULT_NAME);
    e->text[0] = 0;
    e->len = 0;
    e->cursor = 0;
    e->top_line = 0;
    e->left_col = 0;
    e->dirty = 0;
    e->after_save = 0;
    e->pending_open = FS_NONE;
}

void editor_prompt_unsaved(struct wm *wm, int pending_open)
{
    wm->editor.pending_open = pending_open;
    shell_prompt_unsaved(wm, wm->editor.name);
}

void editor_open(struct wm *wm, int node)
{
    struct editor_app *e = &wm->editor;
    if (e->dirty && e->node != node) {
        wm_open(wm, APP_EDITOR);
        editor_prompt_unsaved(wm, node);
        return;
    }
    if (!e->dirty)
        load(wm, node);
    wm_open(wm, APP_EDITOR);
    changed_view(wm);
}

static void finish_pending(struct wm *wm)
{
    struct editor_app *e = &wm->editor;
    if (!e->after_save)
        return;

    int next = e->pending_open;
    e->after_save = 0;
    e->pending_open = FS_NONE;
    if (next != FS_NONE && fs_is_file(wm->fs, next)) {
        load(wm, next);
        changed_view(wm);
    } else {
        wm_close(wm, APP_EDITOR);
    }
}

void editor_save(struct wm *wm)
{
    struct editor_app *e = &wm->editor;
    if (e->node == FS_NONE) {
        shell_prompt_save_as(wm, e->name);
        return;
    }

    int status = fs_write(wm->fs, e->node, e->text, e->len);
    if (status < 0) {
        e->after_save = 0;
        e->pending_open = FS_NONE;
        shell_error(wm, status);
        return;
    }

    e->dirty = 0;
    text_copy(e->name, sizeof e->name, wm->fs->nodes[e->node].name);
    char event[48];
    text_copy(event, sizeof event, "fs:save:");
    text_append(event, sizeof event, e->name);
    wm_trace(wm, event);
    shell_fs_changed(wm);
    finish_pending(wm);
}

void editor_save_as(struct wm *wm, const char *name)
{
    struct editor_app *e = &wm->editor;
    struct fs *fs = wm->fs;
    int dir = e->dir;
    if (!fs_is_dir(fs, dir) || fs_is_within(fs, dir, fs->trash))
        dir = fs->documents;

    int node = fs_create(fs, dir, name, FS_FILE);
    if (node < 0) {
        e->after_save = 0;
        e->pending_open = FS_NONE;
        shell_error(wm, node);
        return;
    }
    e->node = node;
    e->dir = dir;
    editor_save(wm);
}

void editor_resolve_unsaved(struct wm *wm, int choice)
{
    struct editor_app *e = &wm->editor;
    if (choice == 0) {
        e->after_save = 1;
        editor_save(wm);
    } else if (choice == 1) {
        e->dirty = 0;
        e->after_save = 1;
        finish_pending(wm);
    } else {
        e->after_save = 0;
        e->pending_open = FS_NONE;
    }
}

void editor_forget(struct wm *wm, int node)
{
    struct editor_app *e = &wm->editor;
    if (e->node != FS_NONE && fs_is_within(wm->fs, e->node, node)) {
        /* Keep the text on screen as an unsaved document so it is not silently lost. */
        e->node = FS_NONE;
        e->dirty = 1;
    }
    if (e->pending_open != FS_NONE && fs_is_within(wm->fs, e->pending_open, node))
        e->pending_open = FS_NONE;
    if (e->dir != FS_NONE && fs_is_within(wm->fs, e->dir, node))
        e->dir = FS_NONE;
}

void editor_sync_name(struct wm *wm)
{
    struct editor_app *e = &wm->editor;
    if (e->node != FS_NONE && fs_is_file(wm->fs, e->node)) {
        text_copy(e->name, sizeof e->name, wm->fs->nodes[e->node].name);
        e->dir = wm->fs->nodes[e->node].parent;
    }
}

static void insert_char(struct editor_app *e, char value)
{
    if (e->len >= EDITOR_MAX - 1)
        return;
    for (int i = e->len; i > e->cursor; i--)
        e->text[i] = e->text[i - 1];
    e->text[e->cursor++] = value;
    e->text[++e->len] = 0;
    e->dirty = 1;
}

static void delete_at(struct editor_app *e, int position)
{
    if (position < 0 || position >= e->len)
        return;
    for (int i = position; i < e->len - 1; i++)
        e->text[i] = e->text[i + 1];
    e->text[--e->len] = 0;
    e->dirty = 1;
}

static void move_vertical(struct wm *wm, int lines)
{
    struct editor_app *e = &wm->editor;
    int column = e->cursor - line_start(e, e->cursor);
    int target = line_number(e, e->cursor) + lines;
    if (target < 0)
        target = 0;

    int start = line_offset(e, target);
    int end = line_end(e, start);
    e->cursor = start + column > end ? end : start + column;
}

void editor_key(struct wm *wm, int key)
{
    struct editor_app *e = &wm->editor;
    int page = visible_rows(wm_content_rect(wm, APP_EDITOR));

    switch (key) {
    case KEY_CTRL_S:
        editor_save(wm);
        return;
    case KEY_ENTER:
        insert_char(e, '\n');
        break;
    case KEY_BACKSPACE:
        if (e->cursor > 0) {
            e->cursor--;
            delete_at(e, e->cursor);
        }
        break;
    case KEY_DELETE:
        delete_at(e, e->cursor);
        break;
    case KEY_LEFT:
        if (e->cursor > 0)
            e->cursor--;
        break;
    case KEY_RIGHT:
        if (e->cursor < e->len)
            e->cursor++;
        break;
    case KEY_UP:
        move_vertical(wm, -1);
        break;
    case KEY_DOWN:
        move_vertical(wm, 1);
        break;
    case KEY_PGUP:
        move_vertical(wm, -page);
        break;
    case KEY_PGDN:
        move_vertical(wm, page);
        break;
    case KEY_HOME:
        e->cursor = line_start(e, e->cursor);
        break;
    case KEY_END:
        e->cursor = line_end(e, e->cursor);
        break;
    default:
        if (key < 0x20 || key > 0x7e)
            return;
        insert_char(e, (char)key);
        break;
    }
    changed_view(wm);
}

void editor_press(struct wm *wm, int x, int y, int button)
{
    struct editor_app *e = &wm->editor;
    struct rect content = wm_content_rect(wm, APP_EDITOR);
    if (button != BUTTON_LEFT)
        return;

    if (rect_contains(save_button(content), x, y)) {
        editor_save(wm);
        return;
    }
    if (rect_contains(save_as_button(content), x, y)) {
        shell_prompt_save_as(wm, e->name);
        return;
    }

    struct rect area = text_area(content);
    if (!rect_contains(area, x, y))
        return;

    int line = e->top_line + (y - area.y) / FONT_LINE;
    int column = e->left_col + (x - area.x + FONT_ADVANCE / 2) / FONT_ADVANCE;
    int start = line_offset(e, line);
    int end = line_end(e, start);
    e->cursor = start + column > end ? end : start + column;
    changed_view(wm);
}

void editor_draw(struct wm *wm, struct rect content, int focused)
{
    struct canvas *c = wm->screen;
    const struct editor_app *e = &wm->editor;

    canvas_fill(c, (struct rect){ content.x, content.y, content.w, TOOLBAR_H }, UI_PANEL);
    struct rect save = save_button(content);
    struct rect save_as = save_as_button(content);
    canvas_fill(c, save, UI_TITLE);
    canvas_frame(c, save, UI_BORDER);
    canvas_text(c, save.x + 6, save.y + 5, "Save", UI_TEXT);
    canvas_fill(c, save_as, UI_TITLE);
    canvas_frame(c, save_as, UI_BORDER);
    canvas_text(c, save_as.x + 6, save_as.y + 5, "Save As", UI_TEXT);

    struct rect area = text_area(content);
    int rows = visible_rows(content);
    int cols = visible_cols(content);
    int position = line_offset(e, e->top_line);

    for (int row = 0; row < rows; row++) {
        int end = line_end(e, position);
        int start = position + e->left_col;
        if (start < end) {
            char visible[MAX_LINE_DRAW + 1];
            int count = end - start < cols ? end - start : cols;
            for (int i = 0; i < count; i++)
                visible[i] = e->text[start + i];
            visible[count] = 0;
            canvas_text(c, area.x, area.y + row * FONT_LINE, visible, UI_TEXT);
        }
        if (end >= e->len)
            break;
        position = end + 1;
    }

    int line = line_number(e, e->cursor);
    int column = e->cursor - line_start(e, e->cursor);
    if (focused && line >= e->top_line && line < e->top_line + rows && column >= e->left_col &&
        column < e->left_col + cols)
        canvas_fill(c,
                    (struct rect){ area.x + (column - e->left_col) * FONT_ADVANCE - 1,
                                   area.y + (line - e->top_line) * FONT_LINE - 1, 2, FONT_LINE },
                    UI_ACCENT);

    char status[48];
    text_copy(status, sizeof status, "Line ");
    text_append_u64(status, sizeof status, (uint64_t)line + 1);
    text_append(status, sizeof status, ", Col ");
    text_append_u64(status, sizeof status, (uint64_t)column + 1);
    text_append(status, sizeof status, e->dirty ? "   Unsaved" : "");
    struct rect bar = { content.x, content.y + content.h - STATUS_H, content.w, STATUS_H };
    canvas_fill(c, bar, UI_PANEL);
    canvas_fill(c, (struct rect){ bar.x, bar.y, bar.w, 1 }, UI_BORDER);
    ui_text_fit(c, bar.x + 8, bar.y + 6, status, (bar.w - 32) / FONT_ADVANCE, UI_DIM);
}
