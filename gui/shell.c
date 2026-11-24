#include "shell.h"

#include "apps.h"
#include "icons.h"
#include "text.h"

#define ICON_CELL_W 100
#define ICON_CELL_H 84
#define ICON_LABEL_COLS 8
#define DESKTOP_MAX 64
#define SHORTCUTS 6
#define CONTEXT_W 216
#define CONTEXT_ITEM_H 28
#define DIALOG_W 460
#define DIALOG_H 176
#define DIALOG_BUTTON_W 104
#define DIALOG_BUTTON_H 30

struct desktop_entry {
    int icon;
    int app;  /* App to open, or -1 for a filesystem item. */
    int node; /* Filesystem item, or FS_NONE for an app shortcut. */
    const char *label;
};

void ui_text_fit(struct canvas *c, int x, int y, const char *text, int max_chars, uint32_t color)
{
    char buffer[64];
    if (max_chars <= 0)
        return;
    if (max_chars > (int)sizeof buffer - 1)
        max_chars = (int)sizeof buffer - 1;

    if (text_len(text) <= max_chars) {
        canvas_text(c, x, y, text, color);
        return;
    }
    int keep = max_chars > 2 ? max_chars - 2 : max_chars;
    for (int i = 0; i < keep; i++)
        buffer[i] = text[i];
    buffer[keep] = 0;
    if (max_chars > 2)
        text_append(buffer, sizeof buffer, "..");
    canvas_text(c, x, y, buffer, color);
}

static void trace_name(const struct wm *wm, const char *verb, const char *name)
{
    char event[64];
    text_copy(event, sizeof event, verb);
    text_append(event, sizeof event, name);
    wm_trace(wm, event);
}

/* Desktop icons */

static int desktop_entries(const struct wm *wm, struct desktop_entry *out)
{
    const struct fs *fs = wm->fs;
    out[0] = (struct desktop_entry){ ICON_FOLDER, APP_FILES, FS_NONE, "Files" };
    out[1] = (struct desktop_entry){ ICON_TRASH, -1, fs->trash, "Trash" };
    out[2] = (struct desktop_entry){ ICON_TERMINAL, APP_TERMINAL, FS_NONE, "Terminal" };
    out[3] = (struct desktop_entry){ ICON_EDITOR, APP_EDITOR, FS_NONE, "Editor" };
    out[4] = (struct desktop_entry){ ICON_SYSTEM, APP_SYSTEM, FS_NONE, "System" };
    out[5] = (struct desktop_entry){ ICON_BROWSER, APP_BROWSER, FS_NONE, "Browser" };

    int nodes[DESKTOP_MAX - SHORTCUTS];
    int count = fs_list(fs, fs->desktop, nodes, DESKTOP_MAX - SHORTCUTS);
    for (int i = 0; i < count; i++) {
        int node = nodes[i];
        out[SHORTCUTS + i] = (struct desktop_entry){
            fs_is_dir(fs, node) ? ICON_FOLDER : ICON_FILE, -1, node, fs->nodes[node].name,
        };
    }
    return SHORTCUTS + count;
}

static int icon_rows(const struct wm *wm)
{
    int rows = (wm->screen->height - WM_TASKBAR_H - 8) / ICON_CELL_H;
    return rows < 1 ? 1 : rows;
}

static struct rect icon_rect(const struct wm *wm, int index)
{
    int rows = icon_rows(wm);
    return (struct rect){ 8 + (index / rows) * ICON_CELL_W, 8 + (index % rows) * ICON_CELL_H,
                          ICON_CELL_W, ICON_CELL_H };
}

void shell_draw_desktop(struct wm *wm)
{
    struct desktop_entry entries[DESKTOP_MAX];
    int count = desktop_entries(wm, entries);

    for (int i = 0; i < count; i++) {
        struct rect cell = icon_rect(wm, i);
        if (i == wm->icon_selected) {
            canvas_blend(wm->screen, cell, UI_ACCENT, 70);
            canvas_frame(wm->screen, cell, UI_ACCENT);
        }
        icon_draw(wm->screen, entries[i].icon, cell.x + (cell.w - 32) / 2, cell.y + 8, 2);

        int length = text_len(entries[i].label);
        int shown = length > ICON_LABEL_COLS ? ICON_LABEL_COLS : length;
        ui_text_fit(wm->screen, cell.x + (cell.w - shown * FONT_ADVANCE) / 2, cell.y + 52,
                    entries[i].label, ICON_LABEL_COLS, UI_TEXT);
    }
}

static void select_icon(struct wm *wm, int index)
{
    if (wm->icon_selected >= 0)
        wm_invalidate(wm, icon_rect(wm, wm->icon_selected));
    wm->icon_selected = index;
    if (index >= 0)
        wm_invalidate(wm, icon_rect(wm, index));
}

static void activate_entry(struct wm *wm, const struct desktop_entry *entry)
{
    if (entry->app >= 0) {
        if (entry->app == APP_FILES)
            files_open_dir(wm, FS_ROOT);
        wm_open(wm, entry->app);
    } else {
        shell_action(wm, ACTION_OPEN, entry->node, wm->fs->desktop);
    }
}

void shell_desktop_press(struct wm *wm, int x, int y, int button, int clicks)
{
    struct desktop_entry entries[DESKTOP_MAX];
    int count = desktop_entries(wm, entries);
    wm_focus_desktop(wm);

    for (int i = 0; i < count; i++) {
        if (!rect_contains(icon_rect(wm, i), x, y))
            continue;
        select_icon(wm, i);
        if (button == BUTTON_RIGHT) {
            if (entries[i].node != FS_NONE)
                shell_context_node(wm, x, y, entries[i].node);
        } else if (clicks == 2) {
            activate_entry(wm, &entries[i]);
        }
        return;
    }

    select_icon(wm, -1);
    if (button == BUTTON_RIGHT)
        shell_context_dir(wm, x, y, wm->fs->desktop);
}

void shell_desktop_key(struct wm *wm, int key)
{
    struct desktop_entry entries[DESKTOP_MAX];
    int count = desktop_entries(wm, entries);
    int selected = wm->icon_selected;
    int node = selected >= 0 && selected < count ? entries[selected].node : FS_NONE;
    int target = selected;

    switch (key) {
    case KEY_UP:
        target = selected <= 0 ? 0 : selected - 1;
        break;
    case KEY_DOWN:
        target = selected + 1 < count ? selected + 1 : count - 1;
        break;
    case KEY_LEFT:
        target = selected - icon_rows(wm) >= 0 ? selected - icon_rows(wm) : (selected < 0 ? 0 : selected);
        break;
    case KEY_RIGHT:
        target = selected + icon_rows(wm) < count ? selected + icon_rows(wm)
                                                  : (selected < 0 ? 0 : selected);
        break;
    case KEY_ENTER:
        if (selected >= 0 && selected < count)
            activate_entry(wm, &entries[selected]);
        return;
    case KEY_F2:
        if (node != FS_NONE)
            shell_action(wm, ACTION_RENAME, node, wm->fs->desktop);
        return;
    case KEY_DELETE:
        if (node != FS_NONE)
            shell_action(wm, ACTION_TRASH, node, wm->fs->desktop);
        return;
    case KEY_CTRL_C:
        if (node != FS_NONE)
            shell_action(wm, ACTION_COPY, node, wm->fs->desktop);
        return;
    case KEY_CTRL_X:
        if (node != FS_NONE)
            shell_action(wm, ACTION_CUT, node, wm->fs->desktop);
        return;
    case KEY_CTRL_V:
        shell_action(wm, ACTION_PASTE, FS_NONE, wm->fs->desktop);
        return;
    case KEY_CTRL_N:
        shell_action(wm, ACTION_NEW_FOLDER, FS_NONE, wm->fs->desktop);
        return;
    default:
        return;
    }
    if (target != selected)
        select_icon(wm, target);
}

/* Dialogs */

static int dialog_labels(int kind, const char *labels[3])
{
    switch (kind) {
    case DIALOG_RENAME:
        labels[0] = "OK";
        labels[1] = "Cancel";
        return 2;
    case DIALOG_SAVE_AS:
        labels[0] = "Save";
        labels[1] = "Cancel";
        return 2;
    case DIALOG_CONFIRM_DELETE:
        labels[0] = "Delete";
        labels[1] = "Cancel";
        return 2;
    case DIALOG_CONFIRM_EMPTY:
        labels[0] = "Empty";
        labels[1] = "Cancel";
        return 2;
    case DIALOG_CONFIRM_EXIT:
        labels[0] = "Exit";
        labels[1] = "Cancel";
        return 2;
    case DIALOG_UNSAVED:
        labels[0] = "Save";
        labels[1] = "Discard";
        labels[2] = "Cancel";
        return 3;
    default:
        labels[0] = "OK";
        return 1;
    }
}

static int dialog_has_input(int kind)
{
    return kind == DIALOG_RENAME || kind == DIALOG_SAVE_AS;
}

static struct rect dialog_rect(const struct wm *wm)
{
    int height = wm->screen->height - WM_TASKBAR_H;
    return (struct rect){ (wm->screen->width - DIALOG_W) / 2, (height - DIALOG_H) / 2, DIALOG_W,
                          DIALOG_H };
}

static struct rect dialog_input_rect(const struct wm *wm)
{
    struct rect d = dialog_rect(wm);
    return (struct rect){ d.x + 16, d.y + 80, d.w - 32, 30 };
}

static struct rect dialog_button_rect(const struct wm *wm, int index, int count)
{
    struct rect d = dialog_rect(wm);
    int x = d.x + d.w - 16 - (count - index) * (DIALOG_BUTTON_W + 8) + 8;
    return (struct rect){ x, d.y + d.h - DIALOG_BUTTON_H - 12, DIALOG_BUTTON_W, DIALOG_BUTTON_H };
}

static void open_dialog(struct wm *wm, int kind, const char *title, const char *message,
                        const char *input, int node)
{
    struct dialog *d = &wm->dialog;
    d->kind = kind;
    d->node = node;
    d->button = 0;
    d->input_fresh = 1;
    text_copy(d->title, sizeof d->title, title);
    text_copy(d->message, sizeof d->message, message);
    text_copy(d->input, sizeof d->input, input);
    d->input_len = text_len(d->input);
    wm_invalidate(wm, dialog_rect(wm));
    wm_trace(wm, "dialog:open");
}

static void close_dialog(struct wm *wm)
{
    wm->dialog.kind = DIALOG_NONE;
    wm_invalidate(wm, dialog_rect(wm));
}

void shell_error(struct wm *wm, int status)
{
    open_dialog(wm, DIALOG_ERROR, "Error", fs_error_text(status), "", FS_NONE);
    wm_trace(wm, "dialog:error");
}

void shell_request_exit(struct wm *wm)
{
    open_dialog(wm, DIALOG_CONFIRM_EXIT, "Exit to firmware",
                wm->storage_ready ? "Your files are saved to USB." : "Files in memory will be lost.",
                "", FS_NONE);
}

void shell_message(struct wm *wm, const char *title, const char *message)
{
    if (wm->dialog.kind == DIALOG_NONE)
        open_dialog(wm, DIALOG_ERROR, title, message, "", FS_NONE);
}

void shell_notice(struct wm *wm, const char *message)
{
    shell_message(wm, "Storage", message);
}

void shell_prompt_save_as(struct wm *wm, const char *suggested)
{
    open_dialog(wm, DIALOG_SAVE_AS, "Save As", "File name:", suggested, FS_NONE);
}

void shell_prompt_unsaved(struct wm *wm, const char *name)
{
    char message[64];
    text_copy(message, sizeof message, "Save changes to ");
    text_append(message, sizeof message, name);
    text_append(message, sizeof message, "?");
    open_dialog(wm, DIALOG_UNSAVED, "Unsaved changes", message, "", FS_NONE);
}

/* Clears every reference to items about to be permanently deleted. */
static void before_delete(struct wm *wm, int node)
{
    files_forget(wm, node);
    editor_forget(wm, node);
    terminal_forget(wm, node);
    if (wm->clipboard.node != FS_NONE && fs_is_within(wm->fs, wm->clipboard.node, node))
        wm->clipboard.node = FS_NONE;
}

static void dialog_activate(struct wm *wm, int index)
{
    struct dialog d = wm->dialog;
    struct fs *fs = wm->fs;
    close_dialog(wm);

    switch (d.kind) {
    case DIALOG_RENAME:
        if (index == 0) {
            int status = fs_rename(fs, d.node, d.input);
            if (status < 0) {
                shell_error(wm, status);
            } else {
                trace_name(wm, "fs:rename:", d.input);
                shell_fs_changed(wm);
            }
        }
        break;
    case DIALOG_SAVE_AS:
        if (index == 0)
            editor_save_as(wm, d.input);
        else
            editor_resolve_unsaved(wm, 2);
        break;
    case DIALOG_CONFIRM_DELETE:
        if (index == 0) {
            char name[FS_NAME_MAX];
            text_copy(name, sizeof name, fs->nodes[d.node].name);
            before_delete(wm, d.node);
            int status = fs_delete(fs, d.node);
            if (status < 0) {
                shell_error(wm, status);
            } else {
                trace_name(wm, "fs:delete:", name);
                shell_fs_changed(wm);
            }
        }
        break;
    case DIALOG_CONFIRM_EMPTY:
        if (index == 0) {
            int items[FS_MAX_NODES];
            int count = fs_list(fs, fs->trash, items, FS_MAX_NODES);
            for (int i = 0; i < count; i++)
                before_delete(wm, items[i]);
            fs_empty_trash(fs);
            wm_trace(wm, "fs:empty-trash");
            shell_fs_changed(wm);
        }
        break;
    case DIALOG_CONFIRM_EXIT:
        if (index == 0) {
            wm->exit_requested = 1;
            wm_trace(wm, "exit");
        }
        break;
    case DIALOG_UNSAVED:
        editor_resolve_unsaved(wm, index);
        break;
    default:
        break;
    }
}

void shell_dialog_key(struct wm *wm, int key)
{
    struct dialog *d = &wm->dialog;
    const char *labels[3];
    int count = dialog_labels(d->kind, labels);

    switch (key) {
    case KEY_ENTER:
        dialog_activate(wm, d->button);
        return;
    case KEY_ESC:
        dialog_activate(wm, count - 1);
        return;
    case KEY_TAB:
        d->button = (d->button + 1) % count;
        wm_invalidate(wm, dialog_rect(wm));
        return;
    default:
        break;
    }

    if (!dialog_has_input(d->kind)) {
        if (key == KEY_LEFT || key == KEY_RIGHT) {
            d->button = (d->button + (key == KEY_RIGHT ? 1 : count - 1)) % count;
            wm_invalidate(wm, dialog_rect(wm));
        }
        return;
    }

    if (key == KEY_BACKSPACE) {
        if (d->input_fresh) {
            /* The suggested name is selected, so Backspace clears all of it. */
            d->input[0] = 0;
            d->input_len = 0;
            d->input_fresh = 0;
        } else if (d->input_len > 0) {
            d->input[--d->input_len] = 0;
        }
    } else if (key >= 0x20 && key <= 0x7e) {
        if (d->input_fresh) {
            d->input[0] = 0;
            d->input_len = 0;
            d->input_fresh = 0;
        }
        if (d->input_len < FS_NAME_MAX - 1) {
            d->input[d->input_len++] = (char)key;
            d->input[d->input_len] = 0;
        }
    } else {
        return;
    }
    wm_invalidate(wm, dialog_rect(wm));
}

void shell_dialog_press(struct wm *wm, int x, int y)
{
    const char *labels[3];
    int count = dialog_labels(wm->dialog.kind, labels);
    for (int i = 0; i < count; i++) {
        if (rect_contains(dialog_button_rect(wm, i, count), x, y)) {
            dialog_activate(wm, i);
            return;
        }
    }
}

static void draw_dialog(struct wm *wm)
{
    struct canvas *c = wm->screen;
    const struct dialog *d = &wm->dialog;
    struct rect r = dialog_rect(wm);
    const char *labels[3];
    int count = dialog_labels(d->kind, labels);

    canvas_blend(c, (struct rect){ r.x + 8, r.y + 8, r.w, r.h }, 0, 130);
    canvas_fill(c, r, UI_WINDOW);
    canvas_fill(c, (struct rect){ r.x, r.y, r.w, WM_TITLE_H }, UI_TITLE_FOCUS);
    canvas_frame(c, r, UI_ACCENT);
    canvas_text(c, r.x + 10, r.y + 7, d->title, UI_TEXT);
    ui_text_fit(c, r.x + 16, r.y + 48, d->message, (r.w - 32) / FONT_ADVANCE, UI_TEXT);

    if (dialog_has_input(d->kind)) {
        struct rect input = dialog_input_rect(wm);
        canvas_fill(c, input, UI_PANEL);
        canvas_frame(c, input, UI_ACCENT);
        if (d->input_fresh && d->input_len > 0)
            canvas_fill(c,
                        (struct rect){ input.x + 6, input.y + 5, d->input_len * FONT_ADVANCE, 20 },
                        UI_TITLE_FOCUS);
        canvas_text(c, input.x + 6, input.y + 8, d->input, UI_TEXT);
        canvas_fill(c,
                    (struct rect){ input.x + 6 + d->input_len * FONT_ADVANCE, input.y + 6, 2, 18 },
                    UI_ACCENT);
    }

    for (int i = 0; i < count; i++) {
        struct rect button = dialog_button_rect(wm, i, count);
        canvas_fill(c, button, i == d->button ? UI_TITLE_FOCUS : UI_TITLE);
        canvas_frame(c, button, i == d->button ? UI_ACCENT : UI_BORDER);
        canvas_text(c, button.x + (button.w - text_len(labels[i]) * FONT_ADVANCE) / 2, button.y + 8,
                    labels[i], i == d->button ? UI_ACCENT : UI_TEXT);
    }
}

/* Right-click menus */

static struct rect context_rect(const struct wm *wm)
{
    const struct context_menu *m = &wm->context;
    return (struct rect){ m->x, m->y, CONTEXT_W, m->count * CONTEXT_ITEM_H + 8 };
}

static struct rect context_item_rect(const struct wm *wm, int index)
{
    const struct context_menu *m = &wm->context;
    return (struct rect){ m->x + 4, m->y + 4 + index * CONTEXT_ITEM_H, CONTEXT_W - 8,
                          CONTEXT_ITEM_H };
}

static void context_begin(struct wm *wm, int x, int y, int node, int dir)
{
    struct context_menu *m = &wm->context;
    m->x = x;
    m->y = y;
    m->count = 0;
    m->index = 0;
    m->node = node;
    m->dir = dir;
}

static void context_add(struct wm *wm, const char *label, int action)
{
    struct context_menu *m = &wm->context;
    if (m->count < CONTEXT_MAX)
        m->items[m->count++] = (struct menu_item){ label, action };
}

static void context_show(struct wm *wm)
{
    struct context_menu *m = &wm->context;
    if (m->count == 0)
        return;

    struct rect r = context_rect(wm);
    int bottom = wm->screen->height - WM_TASKBAR_H;
    if (r.x + r.w > wm->screen->width)
        m->x = wm->screen->width - r.w;
    if (r.y + r.h > bottom)
        m->y = bottom - r.h;
    if (m->x < 0)
        m->x = 0;
    if (m->y < 0)
        m->y = 0;

    m->open = 1;
    wm_invalidate(wm, (struct rect){ m->x, m->y, r.w + 6, r.h + 6 });
    wm_trace(wm, "context:open");
}

static void context_close(struct wm *wm)
{
    struct rect r = context_rect(wm);
    wm->context.open = 0;
    wm_invalidate(wm, (struct rect){ r.x, r.y, r.w + 6, r.h + 6 });
}

void shell_context_node(struct wm *wm, int x, int y, int node)
{
    const struct fs *fs = wm->fs;
    context_begin(wm, x, y, node, fs->nodes[node].parent);
    context_add(wm, "Open", ACTION_OPEN);

    if (node == fs->trash) {
        context_add(wm, "Empty Trash", ACTION_EMPTY_TRASH);
    } else if (!fs->nodes[node].is_protected) {
        if (fs_is_within(fs, node, fs->trash)) {
            if (fs->nodes[node].parent == fs->trash)
                context_add(wm, "Restore", ACTION_RESTORE);
            context_add(wm, "Delete Forever", ACTION_DELETE);
        } else {
            context_add(wm, "Rename", ACTION_RENAME);
            context_add(wm, "Copy", ACTION_COPY);
            context_add(wm, "Cut", ACTION_CUT);
            context_add(wm, "Move to Trash", ACTION_TRASH);
        }
    }
    context_show(wm);
}

void shell_context_dir(struct wm *wm, int x, int y, int dir)
{
    const struct fs *fs = wm->fs;
    context_begin(wm, x, y, FS_NONE, dir);
    if (dir == fs->trash) {
        context_add(wm, "Empty Trash", ACTION_EMPTY_TRASH);
    } else if (!fs_is_within(fs, dir, fs->trash)) {
        context_add(wm, "New Folder", ACTION_NEW_FOLDER);
        context_add(wm, "New Text File", ACTION_NEW_FILE);
        if (wm->clipboard.node != FS_NONE)
            context_add(wm, "Paste", ACTION_PASTE);
    }
    context_show(wm);
}

static void context_activate(struct wm *wm, int index)
{
    struct context_menu m = wm->context;
    context_close(wm);
    shell_action(wm, m.items[index].action, m.node, m.dir);
}

void shell_context_key(struct wm *wm, int key)
{
    struct context_menu *m = &wm->context;
    switch (key) {
    case KEY_UP:
    case KEY_DOWN:
        m->index = (m->index + (key == KEY_DOWN ? 1 : m->count - 1)) % m->count;
        wm_invalidate(wm, context_rect(wm));
        break;
    case KEY_ENTER:
        context_activate(wm, m->index);
        break;
    case KEY_ESC:
        context_close(wm);
        break;
    default:
        break;
    }
}

void shell_context_hover(struct wm *wm, int x, int y)
{
    struct context_menu *m = &wm->context;
    for (int i = 0; i < m->count; i++) {
        if (i != m->index && rect_contains(context_item_rect(wm, i), x, y)) {
            m->index = i;
            wm_invalidate(wm, context_rect(wm));
        }
    }
}

int shell_context_press(struct wm *wm, int x, int y)
{
    for (int i = 0; i < wm->context.count; i++) {
        if (rect_contains(context_item_rect(wm, i), x, y)) {
            context_activate(wm, i);
            return 1;
        }
    }
    context_close(wm);
    return 0;
}

static void draw_context(struct wm *wm)
{
    struct canvas *c = wm->screen;
    struct rect r = context_rect(wm);
    canvas_blend(c, (struct rect){ r.x + 6, r.y + 6, r.w, r.h }, 0, 110);
    canvas_fill(c, r, UI_WINDOW);
    canvas_frame(c, r, UI_ACCENT);
    for (int i = 0; i < wm->context.count; i++) {
        struct rect item = context_item_rect(wm, i);
        if (i == wm->context.index)
            canvas_fill(c, item, UI_TITLE_FOCUS);
        canvas_text(c, item.x + 8, item.y + 7, wm->context.items[i].label,
                    i == wm->context.index ? UI_ACCENT : UI_TEXT);
    }
}

void shell_draw_overlays(struct wm *wm)
{
    if (wm->context.open)
        draw_context(wm);
    if (wm->dialog.kind != DIALOG_NONE)
        draw_dialog(wm);
}

/* File actions */

void shell_fs_changed(struct wm *wm)
{
    const struct fs *fs = wm->fs;
    if (wm->clipboard.node != FS_NONE &&
        (!fs_is_within(fs, wm->clipboard.node, FS_ROOT) ||
         fs_is_within(fs, wm->clipboard.node, fs->trash)))
        wm->clipboard.node = FS_NONE;

    files_refresh(wm);
    editor_sync_name(wm);
    terminal_sync(wm);
    wm->icon_selected = -1;

    /* Desktop icons can change anywhere in their columns. */
    int height = wm->screen->height - WM_TASKBAR_H;
    int columns = DESKTOP_MAX / icon_rows(wm) + 1;
    wm_invalidate(wm, (struct rect){ 0, 0, 8 + columns * ICON_CELL_W, height });
    for (int app = 0; app < APP_COUNT; app++)
        wm_invalidate_app(wm, app);
}

static void create_item(struct wm *wm, int dir, int type)
{
    struct fs *fs = wm->fs;
    char name[FS_NAME_MAX];
    fs_unique_name(fs, dir, type == FS_DIR ? "New Folder" : "New File.txt", name, sizeof name);

    int node = fs_create(fs, dir, name, type);
    if (node < 0) {
        shell_error(wm, node);
        return;
    }
    trace_name(wm, "fs:new:", name);
    shell_fs_changed(wm);
    files_select_node(wm, node);
    open_dialog(wm, DIALOG_RENAME, "Name", "Enter a name:", name, node);
}

static void paste(struct wm *wm, int dir)
{
    struct fs *fs = wm->fs;
    int source = wm->clipboard.node;
    if (source == FS_NONE)
        return;

    int result;
    if (wm->clipboard.cut) {
        result = fs_move(fs, source, dir);
        if (result >= 0) {
            result = source;
            wm->clipboard.node = FS_NONE;
        }
    } else {
        result = fs_copy(fs, source, dir);
    }
    if (result < 0) {
        shell_error(wm, result);
        return;
    }
    trace_name(wm, "fs:paste:", fs->nodes[result].name);
    shell_fs_changed(wm);
    files_select_node(wm, result);
}

void shell_action(struct wm *wm, int action, int node, int dir)
{
    struct fs *fs = wm->fs;
    int status;
    char message[64];

    switch (action) {
    case ACTION_OPEN:
        if (fs_is_dir(fs, node)) {
            files_open_dir(wm, node);
            wm_open(wm, APP_FILES);
        } else if (fs_is_file(fs, node)) {
            editor_open(wm, node);
        }
        break;
    case ACTION_NEW_FOLDER:
        create_item(wm, dir, FS_DIR);
        break;
    case ACTION_NEW_FILE:
        create_item(wm, dir, FS_FILE);
        break;
    case ACTION_RENAME:
        if (node < 0)
            break;
        if (fs->nodes[node].is_protected || fs_is_within(fs, node, fs->trash))
            shell_error(wm, FS_ERR_PROTECTED);
        else
            open_dialog(wm, DIALOG_RENAME, "Rename", "New name:", fs->nodes[node].name, node);
        break;
    case ACTION_COPY:
    case ACTION_CUT:
        if (node < 0)
            break;
        if (fs->nodes[node].is_protected || fs_is_within(fs, node, fs->trash)) {
            shell_error(wm, FS_ERR_PROTECTED);
            break;
        }
        wm->clipboard.node = node;
        wm->clipboard.cut = action == ACTION_CUT;
        trace_name(wm, action == ACTION_CUT ? "clip:cut:" : "clip:copy:", fs->nodes[node].name);
        wm_invalidate_app(wm, APP_FILES);
        break;
    case ACTION_PASTE:
        paste(wm, dir);
        break;
    case ACTION_TRASH:
        if (node < 0)
            break;
        if (fs_is_within(fs, node, fs->trash) && !fs->nodes[node].is_protected) {
            shell_action(wm, ACTION_DELETE, node, dir);
            break;
        }
        text_copy(message, sizeof message, fs->nodes[node].name);
        status = fs_trash(fs, node);
        if (status < 0) {
            shell_error(wm, status);
        } else {
            trace_name(wm, "fs:trash:", message);
            shell_fs_changed(wm);
        }
        break;
    case ACTION_RESTORE:
        status = fs_restore(fs, node);
        if (status < 0) {
            shell_error(wm, status);
        } else {
            trace_name(wm, "fs:restore:", fs->nodes[node].name);
            shell_fs_changed(wm);
        }
        break;
    case ACTION_DELETE:
        if (node < 0)
            break;
        text_copy(message, sizeof message, "Delete ");
        text_append(message, sizeof message, fs->nodes[node].name);
        text_append(message, sizeof message, " forever?");
        open_dialog(wm, DIALOG_CONFIRM_DELETE, "Delete Forever", message, "", node);
        break;
    case ACTION_EMPTY_TRASH:
        if (fs->nodes[fs->trash].first_child != FS_NONE)
            open_dialog(wm, DIALOG_CONFIRM_EMPTY, "Empty Trash", "Delete everything forever?", "",
                        fs->trash);
        break;
    default:
        break;
    }
}
