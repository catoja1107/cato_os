/*
 * HTML to text-layout conversion for the browser. All input is untrusted, so
 * every buffer is bounded. Scripts and styles are discarded and never run.
 */
#ifndef CATO_WEB_HTML_H
#define CATO_WEB_HTML_H

#include <stdint.h>

#include "url.h"

#define WEB_TEXT_MAX (512 * 1024)
#define WEB_ITEMS_MAX 64000
#define WEB_RUNS_MAX 96000
#define WEB_LINKS_MAX 2048
#define WEB_STRINGS_MAX (160 * 1024)
#define WEB_TITLE_MAX 64

enum web_style {
    WEB_BOLD = 1,
    WEB_HEADING = 2,
    WEB_LINK = 4,
    WEB_CODE = 8,
    WEB_DIM = 16,
    WEB_RULE = 32,
    WEB_BIG = 64,    /* Top-level headings, drawn at twice the size over two lines. */
    WEB_QUOTE = 128, /* Inside a blockquote. */
};

/*
 * WEB_BREAK ends the current line. WEB_NEWLINE does too, and on an empty line it
 * leaves a blank one. WEB_GAP separates paragraphs with one blank line.
 */
enum web_item_kind { WEB_WORD, WEB_BREAK, WEB_NEWLINE, WEB_GAP, WEB_HR };

struct web_item {
    uint32_t text;
    uint16_t length;
    int16_t link;
    uint8_t kind;
    uint8_t style;
    uint8_t indent;
    uint8_t space; /* A space comes before this word. */
};

struct web_run {
    int32_t line;
    uint32_t text;
    uint16_t column;
    uint16_t length;
    int16_t link;
    uint8_t style;
};

struct web_doc {
    char title[WEB_TITLE_MAX];
    int truncated;

    int text_length;
    int item_count;
    int run_count;
    int line_count;
    int columns;
    int link_count;
    int strings_length;

    int link_urls[WEB_LINKS_MAX]; /* Offsets into strings. */
    char strings[WEB_STRINGS_MAX];
    char text[WEB_TEXT_MAX];
    struct web_item items[WEB_ITEMS_MAX];
    struct web_run runs[WEB_RUNS_MAX];
};

/* Columns a run occupies on screen. */
static inline int web_run_width(const struct web_run *run)
{
    return run->length * ((run->style & WEB_BIG) ? 2 : 1);
}

void web_doc_clear(struct web_doc *doc);
void web_parse_html(struct web_doc *doc, const struct url *base, const char *html, int length);
void web_parse_text(struct web_doc *doc, const char *text, int length);
void web_layout(struct web_doc *doc, int columns);
const char *web_link_url(const struct web_doc *doc, int link);

#endif
