/* Desktop icons, file actions, right-click menus, and dialogs. */
#ifndef CATO_GUI_SHELL_H
#define CATO_GUI_SHELL_H

#include "wm.h"

/* Runs a file action. node is the item acted on, and dir is the folder for new items. */
void shell_action(struct wm *wm, int action, int node, int dir);
/* Refreshes every view after the filesystem changes. */
void shell_fs_changed(struct wm *wm);
void shell_error(struct wm *wm, int status);
void shell_request_exit(struct wm *wm);
/* Shows a one-button message unless another dialog is already open. */
void shell_message(struct wm *wm, const char *title, const char *message);
/* A storage message, using shell_message. */
void shell_notice(struct wm *wm, const char *message);
void shell_prompt_save_as(struct wm *wm, const char *suggested);
void shell_prompt_unsaved(struct wm *wm, const char *name);

void shell_context_node(struct wm *wm, int x, int y, int node);
void shell_context_dir(struct wm *wm, int x, int y, int dir);
void shell_context_key(struct wm *wm, int key);
void shell_context_hover(struct wm *wm, int x, int y);
/* Returns 1 if the press was consumed by a menu item. */
int shell_context_press(struct wm *wm, int x, int y);

void shell_dialog_key(struct wm *wm, int key);
void shell_dialog_press(struct wm *wm, int x, int y);

void shell_draw_desktop(struct wm *wm);
void shell_draw_overlays(struct wm *wm);
void shell_desktop_key(struct wm *wm, int key);
void shell_desktop_press(struct wm *wm, int x, int y, int button, int clicks);

/* Draws text limited to max_chars columns, shortening it with ".." if needed. */
void ui_text_fit(struct canvas *c, int x, int y, const char *text, int max_chars, uint32_t color);

#endif
