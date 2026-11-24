/* Host unit tests for Cato Desktop. Built with ASan and UBSan. */
#include <stdio.h>
#include <string.h>

#include "apps.h"
#include "ps2_packet.h"
#include "shell.h"
#include "text.h"
#include "wm.h"

#define SCREEN_W 1280
#define SCREEN_H 800

static int failures;
static uint32_t g_pixels[SCREEN_W * SCREEN_H];
static uint32_t g_wallpaper[SCREEN_W * SCREEN_H];
static struct canvas g_canvas;
static struct wm g_wm;
static struct fs g_fs;
static const struct sysinfo g_info = {
    "Test firmware", "2.7", "Test CPU", "1.0 GiB usable", "1280 x 800 at 1x UI", "Test pointer",
};

#define CHECK(condition)                                                 \
    do {                                                                 \
        if (!(condition)) {                                              \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);  \
            failures++;                                                  \
        }                                                                \
    } while (0)

static struct wm *fresh_wm(int width, int height)
{
    fs_init(&g_fs);
    canvas_init(&g_canvas, g_pixels, width, height);
    wm_init(&g_wm, &g_canvas, g_wallpaper, &g_info, &g_fs, NULL);
    return &g_wm;
}

static void type_text(struct wm *wm, const char *text)
{
    for (; *text; text++)
        wm_key(wm, *text);
}

static void type_line(struct wm *wm, const char *text)
{
    type_text(wm, text);
    wm_key(wm, KEY_ENTER);
}

static void move_to(struct wm *wm, int x, int y)
{
    wm_pointer(wm, x - wm->pointer_x, y - wm->pointer_y, 0);
}

static void click(struct wm *wm, int x, int y, int button)
{
    move_to(wm, x, y);
    wm_pointer(wm, 0, 0, button);
    wm_pointer(wm, 0, 0, 0);
}

static int terminal_contains(const struct wm *wm, const char *needle)
{
    for (int row = 0; row < TERM_ROWS - 1; row++) {
        if (strstr(wm->terminal.lines[row], needle))
            return 1;
    }
    return 0;
}

static int file_equals(const struct fs *fs, const char *path, const char *expected)
{
    char buffer[256];
    int node = fs_resolve(fs, FS_ROOT, path);
    if (node < 0)
        return 0;
    int length = fs_read(fs, node, 0, buffer, sizeof buffer);
    return length == (int)strlen(expected) && memcmp(buffer, expected, (size_t)length) == 0;
}

static void test_rects(void)
{
    struct rect out;
    struct rect a = { 0, 0, 10, 10 };
    struct rect b = { 5, 6, 10, 10 };

    CHECK(rect_intersect(a, b, &out));
    CHECK(out.x == 5 && out.y == 6 && out.w == 5 && out.h == 4);
    CHECK(!rect_intersect(a, (struct rect){ 10, 0, 5, 5 }, &out));

    out = rect_union(a, b);
    CHECK(out.x == 0 && out.y == 0 && out.w == 15 && out.h == 16);
    CHECK(rect_contains(a, 9, 9));
    CHECK(!rect_contains(a, 10, 9));
}

static void test_canvas_clipping(void)
{
    struct canvas c;
    uint32_t pixels[16] = { 0 };

    canvas_init(&c, pixels, 4, 4);
    canvas_set_clip(&c, (struct rect){ 1, 1, 2, 2 });
    canvas_fill(&c, (struct rect){ -10, -10, 100, 100 }, 7);
    CHECK(pixels[0] == 0 && pixels[5] == 7 && pixels[10] == 7 && pixels[15] == 0);

    canvas_blend(&c, (struct rect){ 1, 1, 1, 1 }, RGB(255, 255, 255), 256);
    CHECK(pixels[5] == RGB(255, 255, 255));
    canvas_text(&c, -4, -4, "offscreen text", RGB(1, 2, 3));
}

static int row_has_ink(const uint32_t *pixels, int width, int row)
{
    for (int x = 0; x < width; x++) {
        if (pixels[row * width + x])
            return 1;
    }
    return 0;
}

static void test_descenders(void)
{
    struct canvas c;
    uint32_t pixels[FONT_ADVANCE * (FONT_HEIGHT + 2)] = { 0 };

    canvas_init(&c, pixels, FONT_ADVANCE, FONT_HEIGHT + 2);
    canvas_text(&c, 0, 0, "p", RGB(255, 255, 255));
    CHECK(row_has_ink(pixels, FONT_ADVANCE, FONT_HEIGHT + 1));
    CHECK(!row_has_ink(pixels, FONT_ADVANCE, 3));

    memset(pixels, 0, sizeof pixels);
    canvas_text(&c, 0, 0, "P", RGB(255, 255, 255));
    CHECK(row_has_ink(pixels, FONT_ADVANCE, 0));
    CHECK(!row_has_ink(pixels, FONT_ADVANCE, FONT_HEIGHT + 1));
}

static void test_window_stack(void)
{
    struct wm *wm = fresh_wm(SCREEN_W, SCREEN_H);

    CHECK(wm_focused(wm) == -1);
    wm_open(wm, APP_TERMINAL);
    wm_open(wm, APP_SYSTEM);
    CHECK(wm_focused(wm) == APP_SYSTEM && wm->z_count == 2);

    wm_key(wm, KEY_TAB);
    CHECK(wm_focused(wm) == APP_TERMINAL);

    /* Opening an open app focuses it without creating a duplicate. */
    wm_open(wm, APP_SYSTEM);
    CHECK(wm_focused(wm) == APP_SYSTEM && wm->z_count == 2);

    wm_key(wm, KEY_F4);
    CHECK(!wm->windows[APP_SYSTEM].open && wm_focused(wm) == APP_TERMINAL);
    wm_key(wm, KEY_F4);
    CHECK(wm_focused(wm) == -1 && wm->z_count == 0);

    /* Window keys are harmless with nothing open. */
    wm_key(wm, KEY_TAB);
    wm_key(wm, KEY_F3);
    wm_key(wm, KEY_F4);
    CHECK(!wm->move_mode);
}

static void test_minimize_maximize(void)
{
    struct wm *wm = fresh_wm(SCREEN_W, SCREEN_H);
    wm_open(wm, APP_TERMINAL);
    wm_open(wm, APP_FILES);
    struct rect normal = wm->windows[APP_FILES].frame;

    wm_minimize(wm, APP_FILES);
    CHECK(wm->windows[APP_FILES].minimized && wm_focused(wm) == APP_TERMINAL);
    wm_focus(wm, APP_FILES);
    CHECK(!wm->windows[APP_FILES].minimized && wm_focused(wm) == APP_FILES);

    wm_toggle_maximize(wm, APP_FILES);
    CHECK(wm->windows[APP_FILES].maximized);
    CHECK(wm->windows[APP_FILES].frame.w == SCREEN_W);
    CHECK(wm->windows[APP_FILES].frame.h == SCREEN_H - WM_TASKBAR_H);
    wm_toggle_maximize(wm, APP_FILES);
    CHECK(!wm->windows[APP_FILES].maximized);
    CHECK(wm->windows[APP_FILES].frame.x == normal.x && wm->windows[APP_FILES].frame.w == normal.w);

    /* Fixed-size apps cannot be maximized. */
    wm_toggle_maximize(wm, APP_TERMINAL);
    CHECK(!wm->windows[APP_TERMINAL].maximized);

    wm_key(wm, KEY_F6);
    CHECK(wm_focused(wm) == -1);
    CHECK(wm->windows[APP_FILES].minimized && wm->windows[APP_TERMINAL].minimized);
    wm_key(wm, KEY_TAB);
    CHECK(wm_focused(wm) >= 0);
}

static void test_start_menu(void)
{
    struct wm *wm = fresh_wm(SCREEN_W, SCREEN_H);

    wm_key(wm, KEY_F1);
    CHECK(wm->menu_open);
    wm_key(wm, KEY_ENTER);
    CHECK(!wm->menu_open && wm->windows[APP_FILES].open);

    wm_key(wm, KEY_F1);
    wm_key(wm, KEY_ESC);
    CHECK(!wm->menu_open && !wm->exit_requested);

    /* Exit asks first, because RAM files will be lost. */
    wm_key(wm, KEY_F1);
    wm_key(wm, KEY_UP);
    CHECK(wm->menu_index == 6);
    wm_key(wm, KEY_ENTER);
    CHECK(wm->dialog.kind == DIALOG_CONFIRM_EXIT && !wm->exit_requested);
    wm_key(wm, KEY_ESC);
    CHECK(wm->dialog.kind == DIALOG_NONE && !wm->exit_requested);

    wm_key(wm, KEY_F1);
    wm_key(wm, KEY_UP);
    wm_key(wm, KEY_ENTER);
    wm_key(wm, KEY_ENTER);
    CHECK(wm->exit_requested);
}

static void test_keyboard_move(void)
{
    struct wm *wm = fresh_wm(SCREEN_W, SCREEN_H);
    wm_open(wm, APP_SYSTEM);
    struct rect start = wm->windows[APP_SYSTEM].frame;

    wm_key(wm, KEY_F3);
    CHECK(wm->move_mode);
    wm_key(wm, KEY_LEFT);
    wm_key(wm, KEY_DOWN);
    CHECK(wm->windows[APP_SYSTEM].frame.x == start.x - 16);
    CHECK(wm->windows[APP_SYSTEM].frame.y == start.y + 16);

    for (int i = 0; i < 100; i++)
        wm_key(wm, KEY_UP);
    CHECK(wm->windows[APP_SYSTEM].frame.y == 0);
    wm_key(wm, KEY_ENTER);
    CHECK(!wm->move_mode);
}

static void test_pointer_windows(void)
{
    struct wm *wm = fresh_wm(SCREEN_W, SCREEN_H);
    wm_open(wm, APP_FILES);
    struct rect frame = wm->windows[APP_FILES].frame;

    /* Drag by the title bar. */
    move_to(wm, frame.x + 40, frame.y + 10);
    wm_pointer(wm, 0, 0, BUTTON_LEFT);
    CHECK(wm->drag_app == APP_FILES && !wm->drag_resize);
    wm_pointer(wm, -50, 30, BUTTON_LEFT);
    wm_pointer(wm, 0, 0, 0);
    CHECK(wm->drag_app == -1);
    CHECK(wm->windows[APP_FILES].frame.x == frame.x - 50);
    CHECK(wm->windows[APP_FILES].frame.y == frame.y + 30);

    /* Resize with the corner grip, stopping at the minimum size. */
    frame = wm->windows[APP_FILES].frame;
    move_to(wm, frame.x + frame.w - 4, frame.y + frame.h - 4);
    wm_pointer(wm, 0, 0, BUTTON_LEFT);
    CHECK(wm->drag_app == APP_FILES && wm->drag_resize);
    wm_pointer(wm, 40, 20, BUTTON_LEFT);
    CHECK(wm->windows[APP_FILES].frame.w == frame.w + 40);
    CHECK(wm->windows[APP_FILES].frame.h == frame.h + 20);
    wm_pointer(wm, -2000, -2000, BUTTON_LEFT);
    wm_pointer(wm, 0, 0, 0);
    CHECK(wm->windows[APP_FILES].frame.w == 720 && wm->windows[APP_FILES].frame.h == 320);

    /* Title buttons, from right to left: close, maximize, minimize. */
    frame = wm->windows[APP_FILES].frame;
    click(wm, frame.x + frame.w - 14 - 22, frame.y + 12, BUTTON_LEFT);
    CHECK(wm->windows[APP_FILES].maximized);
    click(wm, SCREEN_W - 14 - 22, 12, BUTTON_LEFT);
    CHECK(!wm->windows[APP_FILES].maximized);
    click(wm, frame.x + frame.w - 14 - 44, frame.y + 12, BUTTON_LEFT);
    CHECK(wm->windows[APP_FILES].minimized);

    /* The taskbar button restores it. */
    click(wm, 150, SCREEN_H - 18, BUTTON_LEFT);
    CHECK(!wm->windows[APP_FILES].minimized && wm_focused(wm) == APP_FILES);
    click(wm, 150, SCREEN_H - 18, BUTTON_LEFT);
    CHECK(wm->windows[APP_FILES].minimized);
    click(wm, 150, SCREEN_H - 18, BUTTON_LEFT);

    click(wm, frame.x + frame.w - 14, frame.y + 12, BUTTON_LEFT);
    CHECK(!wm->windows[APP_FILES].open);

    /* The pointer stays on screen. */
    wm_pointer(wm, -100000, -100000, 0);
    CHECK(wm->pointer_x == 0 && wm->pointer_y == 0);
    wm_pointer(wm, 100000, 100000, 0);
    CHECK(wm->pointer_x == SCREEN_W - 1 && wm->pointer_y == SCREEN_H - 1);
}

static void test_double_click_timing(void)
{
    struct wm *wm = fresh_wm(SCREEN_W, SCREEN_H);

    /* Two slow clicks only select the Files icon. */
    click(wm, 50, 40, BUTTON_LEFT);
    for (int i = 0; i < 60; i++)
        wm_tick(wm);
    click(wm, 50, 40, BUTTON_LEFT);
    CHECK(!wm->windows[APP_FILES].open && wm->icon_selected == 0);

    /* The next quick click completes a double-click. */
    wm_tick(wm);
    click(wm, 50, 40, BUTTON_LEFT);
    CHECK(wm->windows[APP_FILES].open);
}

static void test_desktop_icons(void)
{
    struct wm *wm = fresh_wm(SCREEN_W, SCREEN_H);

    /* Right-clicking empty desktop offers new items. */
    click(wm, 700, 400, BUTTON_RIGHT);
    CHECK(wm->context.open && wm->context.dir == g_fs.desktop);
    wm_key(wm, KEY_DOWN);
    wm_key(wm, KEY_ENTER);
    CHECK(wm->dialog.kind == DIALOG_RENAME);
    type_line(wm, "note.txt");
    CHECK(fs_is_file(&g_fs, fs_resolve(&g_fs, FS_ROOT, "/Desktop/note.txt")));

    /* Keyboard navigation opens the selected icon. */
    wm_focus_desktop(wm);
    wm_key(wm, KEY_DOWN);
    CHECK(wm->icon_selected == 0);
    wm_key(wm, KEY_DOWN);
    wm_key(wm, KEY_DOWN);
    wm_key(wm, KEY_ENTER);
    CHECK(wm->windows[APP_TERMINAL].open && wm_focused(wm) == APP_TERMINAL);

    /* Right-clicking the Trash icon offers its actions. */
    click(wm, 50, 8 + 84 + 30, BUTTON_RIGHT);
    CHECK(wm->context.open && wm->context.node == g_fs.trash);
    CHECK(strcmp(wm->context.items[1].label, "Empty Trash") == 0);
    wm_key(wm, KEY_ESC);
    CHECK(!wm->context.open);
}

static void test_files_workflow(void)
{
    struct wm *wm = fresh_wm(SCREEN_W, SCREEN_H);
    struct fs *fs = &g_fs;

    wm_open(wm, APP_FILES);
    files_open_dir(wm, fs->documents);

    /* Create and name a folder. The first keypress replaces the suggested name. */
    wm_key(wm, KEY_CTRL_N);
    CHECK(wm->dialog.kind == DIALOG_RENAME);
    type_line(wm, "Work");
    int work = fs_lookup(fs, fs->documents, "Work");
    CHECK(fs_is_dir(fs, work) && files_selected(wm) == work);
    CHECK(fs_lookup(fs, fs->documents, "New Folder") == FS_ERR_NOT_FOUND);

    /* Copy and paste makes a uniquely named copy. */
    wm_key(wm, KEY_CTRL_C);
    wm_key(wm, KEY_CTRL_V);
    int copy = fs_lookup(fs, fs->documents, "Work (2)");
    CHECK(fs_is_dir(fs, copy) && files_selected(wm) == copy);

    /* Delete moves to the Trash, and the Trash view can restore it. */
    wm_key(wm, KEY_DELETE);
    CHECK(fs->nodes[copy].parent == fs->trash);
    files_open_dir(wm, fs->trash);
    CHECK(wm->files.count == 1);
    shell_action(wm, ACTION_RESTORE, copy, fs->trash);
    CHECK(fs->nodes[copy].parent == fs->documents);
    CHECK(wm->files.count == 0);

    /* Back returns to Documents. Enter opens the selected folder. */
    wm_key(wm, KEY_BACKSPACE);
    CHECK(wm->files.cwd == FS_ROOT);
    files_open_dir(wm, fs->documents);
    files_select_node(wm, work);
    wm_key(wm, KEY_ENTER);
    CHECK(wm->files.cwd == work);
    wm_key(wm, KEY_BACKSPACE);
    CHECK(wm->files.cwd == fs->documents);

    /* Cut and paste moves the item once. */
    files_select_node(wm, copy);
    wm_key(wm, KEY_CTRL_X);
    files_open_dir(wm, work);
    wm_key(wm, KEY_CTRL_V);
    CHECK(fs->nodes[copy].parent == work);
    CHECK(wm->clipboard.node == FS_NONE);

    /* F2 renames. A duplicate name shows an error and changes nothing. */
    files_open_dir(wm, fs->documents);
    files_select_node(wm, work);
    wm_key(wm, KEY_F2);
    type_line(wm, "Notes.txt");
    CHECK(wm->dialog.kind == DIALOG_ERROR);
    wm_key(wm, KEY_ENTER);
    CHECK(strcmp(fs->nodes[work].name, "Work") == 0);
}

static void test_files_navigation_limits(void)
{
    struct wm *wm = fresh_wm(SCREEN_W, SCREEN_H);
    struct fs *fs = &g_fs;
    char name[16];

    wm_open(wm, APP_FILES);
    files_open_dir(wm, fs->downloads);
    for (int i = 0; i < 60; i++) {
        snprintf(name, sizeof name, "file%02d", i);
        fs_create(fs, fs->downloads, name, FS_FILE);
    }
    files_refresh(wm);

    wm_key(wm, KEY_END);
    CHECK(wm->files.selected == 59 && wm->files.scroll > 0);
    wm_key(wm, KEY_HOME);
    CHECK(wm->files.selected == 0 && wm->files.scroll == 0);
    wm_key(wm, KEY_PGDN);
    CHECK(wm->files.selected > 0);
    wm_key(wm, KEY_PGUP);
    CHECK(wm->files.selected == 0);
    wm_render(wm);
    wm->dirty_count = 0;

    /* The history stays bounded across many folder changes. */
    for (int i = 0; i < FILES_HISTORY * 3; i++)
        files_open_dir(wm, i % 2 ? fs->documents : fs->pictures);
    CHECK(wm->files.history_len <= FILES_HISTORY);
}

static void test_delete_forever_clears_references(void)
{
    struct wm *wm = fresh_wm(SCREEN_W, SCREEN_H);
    struct fs *fs = &g_fs;
    int folder = fs_create(fs, fs->documents, "folder", FS_DIR);
    int file = fs_create(fs, folder, "f.txt", FS_FILE);
    fs_write(fs, file, "precious", 8);

    wm_open(wm, APP_FILES);
    wm_open(wm, APP_TERMINAL);
    wm->terminal.cwd = folder;
    files_open_dir(wm, folder);
    editor_open(wm, file);
    wm->clipboard.node = file;
    wm->clipboard.cut = 0;

    CHECK(fs_trash(fs, folder) == FS_OK);
    shell_fs_changed(wm);
    CHECK(wm->clipboard.node == FS_NONE);

    shell_action(wm, ACTION_DELETE, folder, fs->trash);
    CHECK(wm->dialog.kind == DIALOG_CONFIRM_DELETE);
    wm_key(wm, KEY_ENTER);
    CHECK(fs_lookup(fs, fs->trash, "folder") == FS_ERR_NOT_FOUND);

    /* Open views move somewhere valid, and open text stays recoverable. */
    CHECK(fs_is_dir(fs, wm->files.cwd));
    CHECK(fs_is_dir(fs, wm->terminal.cwd));
    CHECK(wm->editor.node == FS_NONE && wm->editor.dirty);
    CHECK(strncmp(wm->editor.text, "precious", 8) == 0);

    /* A new file reusing the freed slot is not confused with the old one. */
    int reused = fs_create(fs, fs->documents, "new.txt", FS_FILE);
    CHECK(reused >= 0 && wm->editor.node != reused);
    wm_render(wm);
    wm->dirty_count = 0;
}

static void test_empty_trash_dialog(void)
{
    struct wm *wm = fresh_wm(SCREEN_W, SCREEN_H);
    struct fs *fs = &g_fs;
    int file = fs_create(fs, fs->documents, "f.txt", FS_FILE);

    shell_action(wm, ACTION_EMPTY_TRASH, FS_NONE, fs->trash);
    CHECK(wm->dialog.kind == DIALOG_NONE); /* Nothing to empty. */

    fs_trash(fs, file);
    shell_action(wm, ACTION_EMPTY_TRASH, FS_NONE, fs->trash);
    CHECK(wm->dialog.kind == DIALOG_CONFIRM_EMPTY);
    wm_key(wm, KEY_ESC);
    CHECK(fs->nodes[fs->trash].first_child != FS_NONE);

    shell_action(wm, ACTION_EMPTY_TRASH, FS_NONE, fs->trash);
    wm_key(wm, KEY_ENTER);
    CHECK(fs->nodes[fs->trash].first_child == FS_NONE);
}

static void test_editor(void)
{
    struct wm *wm = fresh_wm(SCREEN_W, SCREEN_H);
    struct fs *fs = &g_fs;
    int notes = fs_resolve(fs, FS_ROOT, "/Documents/Notes.txt");

    shell_action(wm, ACTION_OPEN, notes, fs->documents);
    CHECK(wm->windows[APP_EDITOR].open && wm_focused(wm) == APP_EDITOR);
    CHECK(wm->editor.node == notes && !wm->editor.dirty);

    wm_key(wm, KEY_END);
    type_text(wm, "!");
    CHECK(wm->editor.dirty);
    wm_key(wm, KEY_CTRL_S);
    CHECK(!wm->editor.dirty);
    CHECK(file_equals(fs, "/Documents/Notes.txt", "My notes!\n"));

    /* Cursor movement across lines clamps to line lengths. */
    wm_key(wm, KEY_DOWN);
    type_text(wm, "ab");
    wm_key(wm, KEY_ENTER);
    type_text(wm, "c");
    wm_key(wm, KEY_UP);
    wm_key(wm, KEY_END);
    wm_key(wm, KEY_BACKSPACE);
    wm_key(wm, KEY_CTRL_S);
    CHECK(file_equals(fs, "/Documents/Notes.txt", "My notes!\na\nc"));

    /* Delete joins lines. */
    wm_key(wm, KEY_DELETE);
    wm_key(wm, KEY_CTRL_S);
    CHECK(file_equals(fs, "/Documents/Notes.txt", "My notes!\nac"));

    /* Closing with unsaved work asks first. */
    type_text(wm, "z");
    wm_key(wm, KEY_F4);
    CHECK(wm->dialog.kind == DIALOG_UNSAVED && wm->windows[APP_EDITOR].open);
    wm_key(wm, KEY_ESC);
    CHECK(wm->dialog.kind == DIALOG_NONE && wm->windows[APP_EDITOR].open && wm->editor.dirty);

    /* Save from the prompt, then close. */
    wm_key(wm, KEY_F4);
    wm_key(wm, KEY_ENTER);
    CHECK(!wm->windows[APP_EDITOR].open);
    CHECK(file_equals(fs, "/Documents/Notes.txt", "My notes!\nazc"));

    /* Discard closes without saving. */
    editor_open(wm, notes);
    type_text(wm, "discard");
    wm_key(wm, KEY_F4);
    wm_key(wm, KEY_RIGHT);
    wm_key(wm, KEY_ENTER);
    CHECK(!wm->windows[APP_EDITOR].open);
    CHECK(file_equals(fs, "/Documents/Notes.txt", "My notes!\nazc"));
}

static void test_editor_save_as_and_switch(void)
{
    struct wm *wm = fresh_wm(SCREEN_W, SCREEN_H);
    struct fs *fs = &g_fs;
    int notes = fs_resolve(fs, FS_ROOT, "/Documents/Notes.txt");
    int welcome = fs_resolve(fs, FS_ROOT, "/Desktop/Welcome.txt");

    /* A new document goes through Save As, into Documents by default. */
    wm_open(wm, APP_EDITOR);
    type_text(wm, "fresh");
    wm_key(wm, KEY_CTRL_S);
    CHECK(wm->dialog.kind == DIALOG_SAVE_AS);
    type_line(wm, "fresh.txt");
    CHECK(file_equals(fs, "/Documents/fresh.txt", "fresh"));
    CHECK(!wm->editor.dirty && strcmp(wm->editor.name, "fresh.txt") == 0);

    /* Save As refuses to overwrite another file. */
    wm_close(wm, APP_EDITOR);
    wm_open(wm, APP_EDITOR);
    type_text(wm, "other");
    wm_key(wm, KEY_CTRL_S);
    type_line(wm, "Notes.txt");
    CHECK(wm->dialog.kind == DIALOG_ERROR);
    wm_key(wm, KEY_ENTER);
    CHECK(file_equals(fs, "/Documents/Notes.txt", "My notes\n"));
    CHECK(wm->editor.dirty);

    /* Opening another file with unsaved work prompts, and Discard switches. */
    editor_open(wm, notes);
    CHECK(wm->dialog.kind == DIALOG_UNSAVED);
    wm_key(wm, KEY_RIGHT);
    wm_key(wm, KEY_ENTER);
    CHECK(wm->editor.node == notes && !wm->editor.dirty);

    /* Renaming an open file updates the editor title. */
    fs_rename(fs, notes, "Renamed.txt");
    shell_fs_changed(wm);
    CHECK(strcmp(wm->editor.name, "Renamed.txt") == 0);

    editor_open(wm, welcome);
    CHECK(wm->editor.node == welcome);
}

static void test_editor_capacity(void)
{
    struct wm *wm = fresh_wm(SCREEN_W, SCREEN_H);

    wm_open(wm, APP_EDITOR);
    for (int i = 0; i < EDITOR_MAX + 10; i++)
        wm_key(wm, i % 40 == 39 ? KEY_ENTER : 'x');
    CHECK(wm->editor.len == EDITOR_MAX - 1);
    wm_render(wm);
    wm->dirty_count = 0;
    wm_key(wm, KEY_PGUP);
    wm_key(wm, KEY_HOME);
    wm_key(wm, KEY_PGDN);
    wm_render(wm);
    wm->dirty_count = 0;
}

static void test_terminal_files(void)
{
    struct wm *wm = fresh_wm(SCREEN_W, SCREEN_H);
    struct fs *fs = &g_fs;
    wm_open(wm, APP_TERMINAL);

    type_line(wm, "ls");
    CHECK(terminal_contains(wm, "Documents/"));

    type_line(wm, "cd Documents");
    type_line(wm, "pwd");
    CHECK(terminal_contains(wm, "/Documents"));
    type_line(wm, "cat Notes.txt");
    CHECK(terminal_contains(wm, "My notes"));

    type_line(wm, "mkdir Projects");
    CHECK(fs_is_dir(fs, fs_resolve(fs, FS_ROOT, "/Documents/Projects")));
    type_line(wm, "touch \"my file.txt\"");
    CHECK(fs_is_file(fs, fs_resolve(fs, FS_ROOT, "/Documents/my file.txt")));
    type_line(wm, "touch \"my file.txt\"");
    CHECK(!terminal_contains(wm, "already in use"));

    type_line(wm, "cp Notes.txt Projects");
    CHECK(file_equals(fs, "/Documents/Projects/Notes.txt", "My notes\n"));
    type_line(wm, "cp Notes.txt copy.txt");
    CHECK(file_equals(fs, "/Documents/copy.txt", "My notes\n"));
    type_line(wm, "mv copy.txt Projects/moved.txt");
    CHECK(file_equals(fs, "/Documents/Projects/moved.txt", "My notes\n"));
    CHECK(fs_resolve(fs, FS_ROOT, "/Documents/copy.txt") == FS_ERR_NOT_FOUND);

    type_line(wm, "mv Notes.txt Projects/moved.txt");
    CHECK(terminal_contains(wm, "already in use"));
    CHECK(fs_is_file(fs, fs_resolve(fs, FS_ROOT, "/Documents/Notes.txt")));

    type_line(wm, "rm \"my file.txt\"");
    CHECK(fs_lookup(fs, fs->trash, "my file.txt") >= 0);
    type_line(wm, "rm /Documents");
    CHECK(terminal_contains(wm, "protected"));
    type_line(wm, "cd /nope");
    CHECK(terminal_contains(wm, "not found"));
    type_line(wm, "cat Projects");
    CHECK(terminal_contains(wm, "is a folder"));
    type_line(wm, "mkdir");
    CHECK(terminal_contains(wm, "Usage"));
    type_line(wm, "echo hello cat");
    CHECK(terminal_contains(wm, "hello cat"));
    type_line(wm, "nope");
    CHECK(terminal_contains(wm, "Unknown command: nope"));
    type_line(wm, "a b c d e");
    CHECK(terminal_contains(wm, "Too many"));

    type_line(wm, "clear");
    CHECK(!terminal_contains(wm, "Too many"));

    wm_key(wm, 'a');
    wm_key(wm, 'b');
    wm_key(wm, KEY_BACKSPACE);
    CHECK(strcmp(wm->terminal.input, "a") == 0);
    wm_key(wm, KEY_BACKSPACE);
    wm_key(wm, KEY_BACKSPACE);
    CHECK(wm->terminal.input_len == 0);
    for (int i = 0; i < 500; i++)
        wm_key(wm, 'x');
    CHECK(wm->terminal.input_len == (int)sizeof wm->terminal.input - 1);
    wm_key(wm, KEY_ENTER);
    CHECK(wm->terminal.input_len == 0);

    /* edit creates a missing file and focuses the editor. */
    type_line(wm, "edit created.txt");
    CHECK(fs_is_file(fs, fs_resolve(fs, FS_ROOT, "/Documents/created.txt")));
    CHECK(wm_focused(wm) == APP_EDITOR);
    wm_focus(wm, APP_TERMINAL);

    type_line(wm, "exit");
    CHECK(wm->dialog.kind == DIALOG_CONFIRM_EXIT);
    wm_key(wm, KEY_ENTER);
    CHECK(wm->exit_requested);
}

static const struct browser_tab *current_tab(const struct wm *wm)
{
    return &wm->browser.tabs[wm->browser.active];
}

static int open_tabs(const struct wm *wm)
{
    int count = 0;
    for (int i = 0; i < BROWSER_TABS; i++)
        count += wm->browser.tabs[i].used;
    return count;
}

static void test_browser_without_network(void)
{
    struct wm *wm = fresh_wm(SCREEN_W, SCREEN_H);
    char title[48];
    char address[URL_MAX];

    wm_open(wm, APP_BROWSER);
    CHECK(strcmp(browser_page_title(wm), "Start") == 0);
    CHECK(current_tab(wm)->security == SECURITY_INTERNAL);
    CHECK(wm->browser.bookmark_count > 0); /* Defaults, until the user changes them. */
    wm_render(wm);
    wm->dirty_count = 0;

    /* Without a network, both HTTP and HTTPS explain the problem. */
    browser_navigate(wm, "https://example.com/");
    CHECK(strcmp(browser_page_title(wm), "No network") == 0);
    CHECK(strcmp(current_tab(wm)->address, "https://example.com/") == 0);
    CHECK(current_tab(wm)->state == TAB_IDLE);
    browser_navigate(wm, "http://example.com/");
    CHECK(strcmp(browser_page_title(wm), "No network") == 0);

    browser_navigate(wm, "javascript:alert(1)");
    CHECK(strcmp(browser_page_title(wm), "Unsupported link") == 0);
    browser_navigate(wm, "http://user@example.com/");
    CHECK(strcmp(browser_page_title(wm), "Invalid address") == 0);
    browser_navigate(wm, "about:<b>nope");
    CHECK(strcmp(browser_page_title(wm), "Page not found") == 0);

    /* Typed text that is not an address becomes a search. */
    browser_address_for_input("  cats & dogs ", address, sizeof address);
    CHECK(strcmp(address, "https://lite.duckduckgo.com/lite/?q=cats+%26+dogs") == 0);
    browser_address_for_input("example.com/path", address, sizeof address);
    CHECK(strcmp(address, "example.com/path") == 0);
    browser_address_for_input("localhost", address, sizeof address);
    CHECK(strcmp(address, "localhost") == 0);
    browser_address_for_input("10.0.2.2:8000", address, sizeof address);
    CHECK(strcmp(address, "10.0.2.2:8000") == 0);

    /* History moves back through visited pages. */
    browser_navigate(wm, "about:home");
    browser_navigate(wm, "about:network");
    CHECK(strcmp(browser_page_title(wm), "Network status") == 0);
    wm_key(wm, KEY_BACKSPACE);
    CHECK(strcmp(browser_page_title(wm), "Start") == 0);
    CHECK(strcmp(current_tab(wm)->address, "about:home") == 0);

    /* Typing starts editing and replaces the address. Esc keeps the page. */
    wm_key(wm, 'a');
    CHECK(wm->browser.editing && strcmp(wm->browser.edit, "a") == 0);
    type_text(wm, "bout:network");
    wm_key(wm, KEY_ENTER);
    CHECK(!wm->browser.editing && strcmp(browser_page_title(wm), "Network status") == 0);
    wm_key(wm, 'x');
    wm_key(wm, KEY_ESC);
    CHECK(!wm->browser.editing && strcmp(current_tab(wm)->address, "about:network") == 0);

    /* Left and Right choose links, and Enter opens one. */
    wm_key(wm, KEY_RIGHT);
    CHECK(current_tab(wm)->link_index == 0);
    wm_key(wm, KEY_ENTER);
    CHECK(strcmp(browser_page_title(wm), "Start") == 0);

    /* Tabs: open, switch, and close. Each keeps its own page. */
    wm_key(wm, KEY_CTRL_T);
    CHECK(open_tabs(wm) == 2 && wm->browser.active == 1 && wm->browser.editing);
    type_text(wm, "about:network");
    wm_key(wm, KEY_ENTER);
    CHECK(strcmp(browser_page_title(wm), "Network status") == 0);
    wm_key(wm, KEY_F7);
    CHECK(wm->browser.active == 0 && strcmp(browser_page_title(wm), "Start") == 0);
    wm_key(wm, KEY_F8);
    CHECK(wm->browser.active == 1);
    for (int i = 0; i < BROWSER_TABS + 2; i++)
        wm_key(wm, KEY_CTRL_T), wm_key(wm, KEY_ESC);
    CHECK(open_tabs(wm) == BROWSER_TABS);
    for (int i = 0; i < BROWSER_TABS + 2; i++)
        wm_key(wm, KEY_CTRL_W);
    CHECK(open_tabs(wm) == 1 && strcmp(browser_page_title(wm), "Start") == 0);

    /* Bookmarks are saved to a file and toggle off again. */
    browser_navigate(wm, "about:network");
    int before = wm->browser.bookmark_count;
    wm_key(wm, KEY_CTRL_D);
    CHECK(wm->browser.bookmark_count == before + 1);
    CHECK(fs_is_file(&g_fs, fs_resolve(&g_fs, FS_ROOT, "/Documents/Bookmarks.txt")));
    CHECK(file_equals(&g_fs, "/Documents/Bookmarks.txt", "") == 0);
    wm_key(wm, KEY_CTRL_D);
    CHECK(wm->browser.bookmark_count == before);

    /* A reloaded desktop reads the saved bookmarks back. */
    browser_navigate(wm, "about:network");
    wm_key(wm, KEY_CTRL_D);
    canvas_init(&g_canvas, g_pixels, SCREEN_W, SCREEN_H);
    wm_init(&g_wm, &g_canvas, g_wallpaper, &g_info, &g_fs, NULL);
    CHECK(wm->browser.bookmark_count == before + 1);
    CHECK(strcmp(wm->browser.bookmark_url[before], "about:network") == 0);
    CHECK(strcmp(wm->browser.bookmark_title[before], "Network status") == 0);
    wm_open(wm, APP_BROWSER);

    /* Find in page highlights a match. */
    wm_key(wm, KEY_CTRL_F);
    CHECK(wm->browser.finding);
    type_text(wm, "BOOKMARKS");
    wm_key(wm, KEY_ENTER);
    CHECK(wm->browser.find_run >= 0);
    wm_render(wm);
    wm->dirty_count = 0;
    wm_key(wm, KEY_ESC);
    CHECK(!wm->browser.finding);

    /* The address field stays bounded. */
    wm_key(wm, KEY_CTRL_L);
    for (int i = 0; i < URL_MAX * 2; i++)
        wm_key(wm, 'z');
    CHECK(wm->browser.edit_len == URL_MAX - 1);
    wm_render(wm);
    wm->dirty_count = 0;
    wm_key(wm, KEY_ESC);

    app_window_title(wm, APP_BROWSER, title, sizeof title);
    CHECK(strcmp(title, "Start - Browser") == 0);

    browser_wheel(wm, 5);
    wm_key(wm, KEY_END);
    wm_key(wm, KEY_PGUP);
    wm_key(wm, KEY_HOME);
    CHECK(current_tab(wm)->top_line == 0);
    wm_toggle_maximize(wm, APP_BROWSER);
    wm_invalidate_all(wm);
    wm_render(wm);
    wm->dirty_count = 0;
}

static void test_exit_message_matches_storage(void)
{
    struct wm *wm = fresh_wm(SCREEN_W, SCREEN_H);
    shell_request_exit(wm);
    CHECK(strstr(wm->dialog.message, "lost") != NULL);
    wm_key(wm, KEY_ESC);

    wm->storage_ready = 1;
    shell_request_exit(wm);
    CHECK(strstr(wm->dialog.message, "saved to USB") != NULL);
}

static void test_terminal_wrap(void)
{
    struct terminal t;
    char line[TERM_COLS + 11];

    terminal_init(&t);
    memset(line, 'w', sizeof line - 1);
    line[sizeof line - 1] = 0;
    terminal_print(&t, line);
    CHECK((int)strlen(t.lines[TERM_ROWS - 3]) == TERM_COLS);
    CHECK(strlen(t.lines[TERM_ROWS - 2]) == 10);
}

static void test_dirty_list(void)
{
    struct wm *wm = fresh_wm(SCREEN_W, SCREEN_H);
    wm->dirty_count = 0;

    for (int i = 0; i < WM_MAX_DIRTY + 5; i++)
        wm_invalidate(wm, (struct rect){ i * 20, i * 20, 5, 5 });
    CHECK(wm->dirty_count <= WM_MAX_DIRTY);

    wm->dirty_count = 0;
    wm_invalidate(wm, (struct rect){ -50, -50, 10, 10 });
    CHECK(wm->dirty_count == 0);
    wm_invalidate(wm, (struct rect){ SCREEN_W - 5, SCREEN_H - 5, 100, 100 });
    CHECK(wm->dirty_count == 1 && wm->dirty[0].w == 5 && wm->dirty[0].h == 5);
}

static void test_render_all_states(void)
{
    static const int sizes[][2] = { { 1280, 800 }, { 800, 600 }, { 640, 480 } };

    for (unsigned s = 0; s < sizeof sizes / sizeof sizes[0]; s++) {
        struct wm *wm = fresh_wm(sizes[s][0], sizes[s][1]);
        wm_open(wm, APP_TERMINAL);
        wm_open(wm, APP_SYSTEM);
        wm_open(wm, APP_EDITOR);
        wm_open(wm, APP_BROWSER);
        wm_open(wm, APP_FILES);
        wm_set_clock(wm, "2026-11-23 12:34");
        wm_pointer(wm, 100000, 100000, 0);
        wm_key(wm, KEY_F1);
        wm_render(wm);
        wm->dirty_count = 0;
        wm_key(wm, KEY_ESC);

        /* Menus and dialogs near the screen edges stay in bounds. */
        click(wm, sizes[s][0] - 2, sizes[s][1] - WM_TASKBAR_H - 2, BUTTON_RIGHT);
        wm_invalidate_all(wm);
        wm_render(wm);
        wm->dirty_count = 0;
        wm_key(wm, KEY_ESC);
        wm->context.open = 0;

        wm_key(wm, KEY_F2);
        shell_request_exit(wm);
        wm_invalidate_all(wm);
        wm_render(wm);
        wm->dirty_count = 0;
        wm_key(wm, KEY_ESC);

        wm_toggle_maximize(wm, APP_FILES);
        wm_invalidate_all(wm);
        wm_render(wm);
        wm->dirty_count = 0;
        wm_toggle_maximize(wm, APP_FILES);

        wm_key(wm, KEY_F3);
        for (int i = 0; i < 200; i++)
            wm_key(wm, KEY_LEFT);
        for (int i = 0; i < 200; i++)
            wm_key(wm, KEY_DOWN);
        wm_key(wm, KEY_ENTER);
        wm_invalidate_all(wm);
        wm_render(wm);
        wm->dirty_count = 0;
    }
}

static void test_ps2_packets(void)
{
    struct ps2_motion m;

    CHECK(!ps2_packet_decode((const uint8_t[]){ 0x00, 1, 1 }, 3, &m));

    CHECK(ps2_packet_decode((const uint8_t[]){ 0x09, 5, 3 }, 3, &m));
    CHECK(m.dx == 5 && m.dy == -3 && m.left == 1 && m.right == 0 && m.wheel == 0);

    CHECK(ps2_packet_decode((const uint8_t[]){ 0x0a, 0, 0 }, 3, &m));
    CHECK(m.left == 0 && m.right == 1);

    /* Negative X and negative Y, where negative Y means up on the device. */
    CHECK(ps2_packet_decode((const uint8_t[]){ 0x38, 0xfb, 0xfd }, 3, &m));
    CHECK(m.dx == -5 && m.dy == 3 && m.left == 0);

    /* Overflow discards movement but keeps the button state. */
    CHECK(ps2_packet_decode((const uint8_t[]){ 0x49, 200, 200 }, 3, &m));
    CHECK(m.dx == 0 && m.dy == 0 && m.left == 1);

    /* IntelliMouse packets carry the wheel in a fourth byte. */
    CHECK(ps2_packet_decode((const uint8_t[]){ 0x08, 0, 0, 0x01 }, 4, &m));
    CHECK(m.wheel == 1);
    CHECK(ps2_packet_decode((const uint8_t[]){ 0x08, 0, 0, 0xff }, 4, &m));
    CHECK(m.wheel == -1);
    CHECK(ps2_packet_decode((const uint8_t[]){ 0x08, 0, 0, 0x40 }, 4, &m));
    CHECK(m.wheel == 0);
}

static void test_text_helpers(void)
{
    char buffer[8] = "";

    text_append(buffer, sizeof buffer, "abc");
    text_append_u64(buffer, sizeof buffer, 12345678);
    CHECK(strcmp(buffer, "abc1234") == 0);

    text_copy(buffer, sizeof buffer, "");
    text_append_2d(buffer, sizeof buffer, 7);
    CHECK(strcmp(buffer, "07") == 0);

    CHECK(text_equal("same", "same") && !text_equal("same", "sam"));
    CHECK(text_starts("echo hi", "echo ") && !text_starts("ech", "echo"));
}

int main(void)
{
    test_rects();
    test_canvas_clipping();
    test_descenders();
    test_window_stack();
    test_minimize_maximize();
    test_start_menu();
    test_keyboard_move();
    test_pointer_windows();
    test_double_click_timing();
    test_desktop_icons();
    test_files_workflow();
    test_files_navigation_limits();
    test_delete_forever_clears_references();
    test_empty_trash_dialog();
    test_editor();
    test_editor_save_as_and_switch();
    test_editor_capacity();
    test_terminal_files();
    test_browser_without_network();
    test_exit_message_matches_storage();
    test_terminal_wrap();
    test_dirty_list();
    test_render_all_states();
    test_ps2_packets();
    test_text_helpers();

    if (failures) {
        printf("%d GUI check(s) failed\n", failures);
        return 1;
    }
    puts("GUI unit tests passed");
    return 0;
}
