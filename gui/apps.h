#ifndef CATO_GUI_APPS_H
#define CATO_GUI_APPS_H

#include "wm.h"

const char *app_title(int app);
const char *app_slug(int app);
void app_window_title(const struct wm *wm, int app, char *out, int capacity);
void app_default_size(int app, int *width, int *height);
void app_min_size(int app, int *width, int *height);
int app_resizable(int app);
void app_resized(struct wm *wm, int app);
void app_draw(struct wm *wm, int app, struct rect content, int focused);
void app_key(struct wm *wm, int app, int key);
void app_press(struct wm *wm, int app, int x, int y, int button, int clicks);
void app_hover(struct wm *wm, int app, int x, int y);
void app_wheel(struct wm *wm, int app, int notches);

/* Files */
void files_init(struct wm *wm);
void files_refresh(struct wm *wm);
void files_open_dir(struct wm *wm, int dir);
void files_select_node(struct wm *wm, int node);
int files_selected(const struct wm *wm);
void files_forget(struct wm *wm, int node);
void files_draw(struct wm *wm, struct rect content, int focused);
void files_key(struct wm *wm, int key);
void files_press(struct wm *wm, int x, int y, int button, int clicks);

/* Text Editor */
void editor_reset(struct wm *wm);
void editor_open(struct wm *wm, int node);
void editor_save(struct wm *wm);
void editor_save_as(struct wm *wm, const char *name);
void editor_prompt_unsaved(struct wm *wm, int pending_open);
/* choice is 0 to save, 1 to discard, and 2 to cancel. */
void editor_resolve_unsaved(struct wm *wm, int choice);
void editor_forget(struct wm *wm, int node);
void editor_sync_name(struct wm *wm);
void editor_ensure_visible(struct wm *wm);
void editor_draw(struct wm *wm, struct rect content, int focused);
void editor_key(struct wm *wm, int key);
void editor_press(struct wm *wm, int x, int y, int button);

/* Terminal */
void terminal_init(struct terminal *terminal);
void terminal_print(struct terminal *terminal, const char *text);
void terminal_forget(struct wm *wm, int node);
void terminal_sync(struct wm *wm);
void terminal_draw(struct wm *wm, struct rect content, int focused);
void terminal_key(struct wm *wm, int key);
void terminal_tick(struct wm *wm);
void terminal_cancel_ping(struct wm *wm);

/* Web Browser */
void browser_init(struct wm *wm);
void browser_navigate(struct wm *wm, const char *text);
void browser_tick(struct wm *wm);
void browser_resized(struct wm *wm);
const char *browser_page_title(const struct wm *wm);
void browser_draw(struct wm *wm, struct rect content, int focused);
void browser_key(struct wm *wm, int key);
void browser_press(struct wm *wm, int x, int y, int button);
void browser_hover(struct wm *wm, int x, int y);
void browser_wheel(struct wm *wm, int notches);
/* Address text to use for a typed entry: a URL, or a web search for other text. */
void browser_address_for_input(const char *input, char *out, int capacity);

#endif
