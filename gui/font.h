#ifndef CATO_GUI_FONT_H
#define CATO_GUI_FONT_H

#include <stdint.h>

#define FONT_GLYPH_COLS 5
#define FONT_GLYPH_ROWS 7

/* Returns 7 rows for an ASCII character. Bit 4 is the leftmost column. */
const uint8_t *font_glyph(char c);

/* Returns 1 for letters drawn one row lower so their tails drop below the baseline. */
int font_descends(char c);

#endif
