#ifndef CATO_GUI_ICONS_H
#define CATO_GUI_ICONS_H

#include "canvas.h"

enum icon_kind {
    ICON_FOLDER,
    ICON_FILE,
    ICON_TRASH,
    ICON_TERMINAL,
    ICON_EDITOR,
    ICON_SYSTEM,
    ICON_BROWSER,
};

/* Draws a 16x16 icon multiplied by scale. */
void icon_draw(struct canvas *c, int kind, int x, int y, int scale);

#endif
