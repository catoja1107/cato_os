/* Cato Desktop window manager and shared UI state. It has no firmware dependencies. */
#ifndef CATO_GUI_WM_H
#define CATO_GUI_WM_H

#include "canvas.h"
#include "fs.h"
#include "url.h"

struct net;

enum gui_key {
    KEY_CTRL_C = 3,
    KEY_CTRL_D = 4,
    KEY_CTRL_F = 6,
    KEY_BACKSPACE = 8,
    KEY_TAB = 9,
    KEY_CTRL_L = 12,
    KEY_ENTER = 13,
    KEY_CTRL_N = 14,
    KEY_CTRL_S = 19,
    KEY_CTRL_T = 20,
    KEY_CTRL_V = 22,
    KEY_CTRL_W = 23,
    KEY_CTRL_X = 24,
    KEY_UP = 0x100,
    KEY_DOWN,
    KEY_LEFT,
    KEY_RIGHT,
    KEY_HOME,
    KEY_END,
    KEY_PGUP,
    KEY_PGDN,
    KEY_DELETE,
    KEY_F1,
    KEY_F2,
    KEY_F3,
    KEY_F4,
    KEY_F5,
    KEY_F6,
    KEY_F7,
    KEY_F8,
    KEY_ESC,
};

enum app_id { APP_FILES, APP_EDITOR, APP_TERMINAL, APP_SYSTEM, APP_BROWSER, APP_COUNT };
enum pointer_button { BUTTON_LEFT = 1, BUTTON_RIGHT = 2 };

enum shell_action {
    ACTION_NONE,
    ACTION_OPEN,
    ACTION_NEW_FOLDER,
    ACTION_NEW_FILE,
    ACTION_RENAME,
    ACTION_COPY,
    ACTION_CUT,
    ACTION_PASTE,
    ACTION_TRASH,
    ACTION_RESTORE,
    ACTION_DELETE,
    ACTION_EMPTY_TRASH,
};

enum dialog_kind {
    DIALOG_NONE,
    DIALOG_RENAME,
    DIALOG_SAVE_AS,
    DIALOG_CONFIRM_DELETE,
    DIALOG_CONFIRM_EMPTY,
    DIALOG_CONFIRM_EXIT,
    DIALOG_UNSAVED,
    DIALOG_ERROR,
};

#define TERM_COLS 48
#define TERM_ROWS 14 /* Includes the input row. */
#define WM_MAX_DIRTY 16
#define WM_TITLE_H 28
#define WM_TASKBAR_H 36
#define FILES_HISTORY 16
#define EDITOR_MAX 16384
#define CONTEXT_MAX 8
#define BROWSER_HISTORY 16
#define BROWSER_TABS 4
#define BROWSER_TITLE_MAX 64
#define BOOKMARKS_MAX 12
#define BOOKMARK_TITLE_MAX 40
#define FIND_MAX 48
#define PING_HOST_MAX 64

#define UI_ACCENT RGB(232, 176, 75)
#define UI_TEXT RGB(232, 230, 225)
#define UI_DIM RGB(138, 136, 146)
#define UI_PANEL RGB(18, 18, 24)
#define UI_WINDOW RGB(24, 24, 30)
#define UI_TITLE RGB(36, 36, 44)
#define UI_TITLE_FOCUS RGB(58, 48, 34)
#define UI_BORDER RGB(64, 64, 74)

struct sysinfo {
    char firmware[48];
    char uefi[16];
    char cpu[52];
    char memory[24];
    char display[32];
    char pointer[24];
};

struct terminal {
    char lines[TERM_ROWS - 1][TERM_COLS + 1];
    char input[TERM_COLS - 2]; /* The prompt and cursor take the other columns. */
    int input_len;
    int cwd;
    int ping_phase;
    char ping_host[PING_HOST_MAX];
};

enum tab_state { TAB_IDLE, TAB_QUEUED, TAB_LOADING };
enum tab_security { SECURITY_INTERNAL, SECURITY_PLAIN, SECURITY_VERIFIED };

struct browser_tab {
    int used;
    int state;
    int security;     /* How the current page arrived. */
    int tls_version;
    int redirects;
    int top_line;
    int link_index;   /* Link selected from the keyboard, or -1. */
    char address[URL_MAX];
    char title[BROWSER_TITLE_MAX];
    char status[96];
    char history[BROWSER_HISTORY][URL_MAX];
    int history_count;
    int history_pos;
    struct url target; /* Address being loaded. */
};

struct browser_app {
    struct browser_tab tabs[BROWSER_TABS];
    int active;
    int loading_tab; /* Tab that owns the network connection, or -1. */
    int fetching;

    char edit[URL_MAX];
    int edit_len;
    int editing;
    int edit_fresh; /* The first typed character replaces the address. */

    int finding;
    char find[FIND_MAX];
    int find_len;
    int find_run; /* Run holding the current match, or -1. */

    int hover_link;
    int bookmark_count;
    char bookmark_url[BOOKMARKS_MAX][URL_MAX];
    char bookmark_title[BOOKMARKS_MAX][BOOKMARK_TITLE_MAX];
};

struct files_app {
    int cwd;
    int entries[FS_MAX_NODES];
    int count;
    int selected; /* Index into entries, or -1. */
    int scroll;
    int history[FILES_HISTORY];
    int history_len;
};

struct editor_app {
    int node; /* FS_NONE for an unsaved document. */
    int dir;  /* Folder used by Save As. */
    char name[FS_NAME_MAX];
    char text[EDITOR_MAX];
    int len;
    int cursor;
    int top_line;
    int left_col;
    int dirty;
    int after_save;   /* Finish a pending close or open once saved. */
    int pending_open; /* File to open after saving, or FS_NONE to close. */
};

struct clipboard {
    int node;
    int cut;
};

struct menu_item {
    const char *label;
    int action;
};

struct context_menu {
    int open;
    int x;
    int y;
    int count;
    int index;
    int node;
    int dir;
    struct menu_item items[CONTEXT_MAX];
};

struct dialog {
    int kind;
    int node;
    int button;
    int input_fresh; /* The first typed character replaces the suggested text. */
    int input_len;
    char title[24];
    char message[64];
    char input[FS_NAME_MAX];
};

struct window {
    int open;
    int minimized;
    int maximized;
    struct rect frame;
    struct rect restore;
};

struct wm {
    struct canvas *screen;
    const uint32_t *wallpaper;
    const struct sysinfo *info;
    struct fs *fs;
    struct net *net;      /* NULL when the platform has no network support. */
    int storage_ready;    /* Files are being saved to the approved USB drive. */
    void (*trace)(const char *event);

    struct window windows[APP_COUNT];
    int z[APP_COUNT]; /* Open windows from bottom to top. */
    int z_count;
    int desktop_focus;

    struct files_app files;
    struct editor_app editor;
    struct terminal terminal;
    struct browser_app browser;
    struct clipboard clipboard;
    struct context_menu context;
    struct dialog dialog;
    int icon_selected;
    char clock[17];

    int menu_open;
    int menu_index;
    int move_mode;

    int pointer_x;
    int pointer_y;
    int pointer_visible;
    int buttons;
    int drag_app; /* -1 when no window is being dragged. */
    int drag_resize;
    int drag_dx;
    int drag_dy;

    uint32_t ticks;
    uint32_t last_click;
    int click_armed;
    int click_x;
    int click_y;

    int exit_requested;
    struct rect dirty[WM_MAX_DIRTY];
    int dirty_count;
};

void wm_init(struct wm *wm, struct canvas *screen, const uint32_t *wallpaper,
             const struct sysinfo *info, struct fs *fs, void (*trace)(const char *event));
void wm_open(struct wm *wm, int app);
void wm_close(struct wm *wm, int app);
/* Closes a window, first asking about unsaved work if needed. */
void wm_request_close(struct wm *wm, int app);
void wm_focus(struct wm *wm, int app);
void wm_focus_desktop(struct wm *wm);
void wm_minimize(struct wm *wm, int app);
void wm_toggle_maximize(struct wm *wm, int app);
void wm_show_desktop(struct wm *wm);
int wm_focused(const struct wm *wm);
struct rect wm_content_rect(const struct wm *wm, int app);

void wm_key(struct wm *wm, int key);
/* buttons is a mask of pointer_button values. */
void wm_pointer(struct wm *wm, int dx, int dy, int buttons);
/* Scroll wheel notches. Positive scrolls down. */
void wm_wheel(struct wm *wm, int notches);
/* Advances UI time. Call once per frame. */
void wm_tick(struct wm *wm);
void wm_set_clock(struct wm *wm, const char *clock);

void wm_invalidate(struct wm *wm, struct rect r);
void wm_invalidate_app(struct wm *wm, int app);
void wm_invalidate_all(struct wm *wm);
/* Redraws every dirty rectangle. The caller presents them and resets dirty_count. */
void wm_render(struct wm *wm);
void wm_trace(const struct wm *wm, const char *event);

#endif
