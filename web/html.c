#include "html.h"

#include "text.h"

#define WORD_MAX 1024
#define LIST_DEPTH 8
#define TAG_NAME_MAX 16
#define ALT_MAX 96
#define MAX_INDENT 24

struct builder {
    struct web_doc *doc;
    const struct url *base;
    int bold;
    int heading;
    int code;
    int pre;
    int dim;
    int big;
    int quote;
    int link;
    int indent;
    int space;
    int word_start;
    int list_depth;
    int list_ordered[LIST_DEPTH];
    int list_number[LIST_DEPTH];
};

struct tag {
    char name[TAG_NAME_MAX];
    int closing;
    char href[URL_MAX];
    char alt[ALT_MAX];
};

static char lower(char c)
{
    return c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c;
}

static int is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f';
}

static int is_alpha(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static int one_of(const char *name, const char *const *list)
{
    for (; *list; list++) {
        if (text_equal(name, *list))
            return 1;
    }
    return 0;
}

void web_doc_clear(struct web_doc *doc)
{
    doc->title[0] = 0;
    doc->truncated = 0;
    doc->text_length = 0;
    doc->item_count = 0;
    doc->run_count = 0;
    doc->line_count = 0;
    doc->columns = 0;
    doc->link_count = 0;
    doc->strings_length = 0;
}

const char *web_link_url(const struct web_doc *doc, int link)
{
    if (link < 0 || link >= doc->link_count)
        return "";
    return doc->strings + doc->link_urls[link];
}

static int current_style(const struct builder *b)
{
    return (b->bold ? WEB_BOLD : 0) | (b->heading ? WEB_HEADING : 0) |
           (b->link >= 0 ? WEB_LINK : 0) | ((b->code || b->pre) ? WEB_CODE : 0) |
           (b->dim ? WEB_DIM : 0) | (b->big ? WEB_BIG : 0) | (b->quote ? WEB_QUOTE : 0);
}

static struct web_item *add_item(struct builder *b, int kind)
{
    struct web_doc *doc = b->doc;
    if (doc->item_count >= WEB_ITEMS_MAX) {
        doc->truncated = 1;
        return 0;
    }
    struct web_item *item = &doc->items[doc->item_count++];
    item->text = 0;
    item->length = 0;
    item->link = -1;
    item->kind = (uint8_t)kind;
    item->style = 0;
    item->indent = (uint8_t)(b->indent > MAX_INDENT ? MAX_INDENT : b->indent);
    item->space = 0;
    return item;
}

static void flush_word(struct builder *b)
{
    if (b->word_start < 0)
        return;
    struct web_doc *doc = b->doc;
    struct web_item *item = add_item(b, WEB_WORD);
    if (item) {
        item->text = (uint32_t)b->word_start;
        item->length = (uint16_t)(doc->text_length - b->word_start);
        item->link = (int16_t)b->link;
        item->style = (uint8_t)current_style(b);
        item->space = (uint8_t)b->space;
    }
    b->word_start = -1;
    b->space = 0;
}

static void word_char(struct builder *b, char c)
{
    struct web_doc *doc = b->doc;
    if (doc->text_length >= WEB_TEXT_MAX) {
        doc->truncated = 1;
        return;
    }
    if (b->word_start < 0)
        b->word_start = doc->text_length;
    doc->text[doc->text_length++] = c;
    if (doc->text_length - b->word_start >= WORD_MAX)
        flush_word(b);
}

static void add_break(struct builder *b, int kind)
{
    flush_word(b);
    add_item(b, kind);
    b->space = 0;
}

static void text_char(struct builder *b, char c)
{
    if (b->pre) {
        if (c == '\n') {
            add_break(b, WEB_NEWLINE);
        } else if (c == '\t') {
            for (int i = 0; i < 4; i++)
                word_char(b, ' ');
        } else if (c != '\r') {
            word_char(b, c);
        }
    } else if (is_space(c)) {
        flush_word(b);
        b->space = 1;
    } else {
        word_char(b, c);
    }
}

static void text_string(struct builder *b, const char *text)
{
    for (; *text; text++)
        text_char(b, *text);
}

/* The bitmap font is ASCII, so common Unicode characters map to close equivalents. */
static void put_codepoint(struct builder *b, uint32_t cp)
{
    static const char LATIN[] =
        "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYTsaaaaaaaceeeeiiiidnooooo/ouuuuyty";

    if (cp == '\t' || cp == '\n' || cp == '\r' || (cp >= 0x20 && cp < 0x7f)) {
        text_char(b, (char)cp);
    } else if (cp >= 0xc0 && cp <= 0xff) {
        text_char(b, LATIN[cp - 0xc0]);
    } else {
        switch (cp) {
        case 0xa0:
        case 0x2002:
        case 0x2003:
        case 0x2009:
            text_char(b, ' ');
            break;
        case 0xa9:
            text_string(b, "(c)");
            break;
        case 0xae:
            text_string(b, "(R)");
            break;
        case 0x2122:
            text_string(b, "(TM)");
            break;
        case 0x2018:
        case 0x2019:
            text_char(b, '\'');
            break;
        case 0x201c:
        case 0x201d:
        case 0xab:
        case 0xbb:
            text_char(b, '"');
            break;
        case 0x2010:
        case 0x2011:
        case 0x2012:
        case 0x2013:
        case 0x2014:
        case 0x2212:
            text_char(b, '-');
            break;
        case 0x2026:
            text_string(b, "...");
            break;
        case 0x2022:
        case 0xb7:
            text_char(b, '*');
            break;
        case 0x200b:
        case 0xfeff:
        case 0xad:
            break;
        default:
            text_char(b, '?');
            break;
        }
    }
}

/* Decodes one UTF-8 sequence. Invalid bytes are treated as Latin-1. */
static int decode_utf8(const char *s, int i, int n, uint32_t *out)
{
    unsigned char c = (unsigned char)s[i];
    int extra = c >= 0xf0 && c <= 0xf4 ? 3 : c >= 0xe0 && c < 0xf0 ? 2 : c >= 0xc2 && c < 0xe0 ? 1 : 0;
    if (extra == 0 || i + extra >= n) {
        *out = c;
        return i + 1;
    }

    uint32_t cp = c & (0x3f >> extra);
    for (int k = 1; k <= extra; k++) {
        unsigned char next = (unsigned char)s[i + k];
        if ((next & 0xc0) != 0x80) {
            *out = c;
            return i + 1;
        }
        cp = (cp << 6) | (next & 0x3f);
    }
    *out = cp;
    return i + extra + 1;
}

/* Decodes an entity at s[i] == '&'. Returns the next index and sets *cp. */
static int decode_entity(const char *s, int i, int n, uint32_t *cp)
{
    static const struct {
        const char *name;
        uint16_t value;
    } NAMED[] = {
        { "amp", '&' },      { "lt", '<' },        { "gt", '>' },       { "quot", '"' },
        { "apos", '\'' },    { "nbsp", 0xa0 },     { "copy", 0xa9 },    { "reg", 0xae },
        { "trade", 0x2122 }, { "mdash", 0x2014 },  { "ndash", 0x2013 }, { "hellip", 0x2026 },
        { "lsquo", 0x2018 }, { "rsquo", 0x2019 },  { "ldquo", 0x201c }, { "rdquo", 0x201d },
        { "bull", 0x2022 },  { "middot", 0xb7 },   { "laquo", 0xab },   { "raquo", 0xbb },
    };

    *cp = '&';
    int end = i + 1;
    while (end < n && end - i <= 10 && s[end] != ';' && !is_space(s[end]) && s[end] != '&' &&
           s[end] != '<')
        end++;
    if (end >= n || s[end] != ';' || end == i + 1)
        return i + 1;

    if (s[i + 1] == '#') {
        int k = i + 2;
        int base = 10;
        uint32_t value = 0;
        if (k < end && (s[k] == 'x' || s[k] == 'X')) {
            base = 16;
            k++;
        }
        if (k >= end)
            return i + 1;
        for (; k < end; k++) {
            char c = lower(s[k]);
            int digit = c >= '0' && c <= '9' ? c - '0' : (base == 16 && c >= 'a' && c <= 'f') ? c - 'a' + 10 : -1;
            if (digit < 0)
                return i + 1;
            value = value * (uint32_t)base + (uint32_t)digit;
            if (value > 0x10ffff)
                return i + 1;
        }
        *cp = value;
        return end + 1;
    }

    char name[10];
    int length = end - i - 1;
    if (length >= (int)sizeof name)
        return i + 1;
    for (int k = 0; k < length; k++)
        name[k] = s[i + 1 + k];
    name[length] = 0;
    for (unsigned k = 0; k < sizeof NAMED / sizeof NAMED[0]; k++) {
        if (text_equal(name, NAMED[k].name)) {
            *cp = NAMED[k].value;
            return end + 1;
        }
    }
    return i + 1;
}

/* Decodes entities in an attribute value in place. Only ASCII is kept. */
static void decode_attribute(char *value)
{
    int n = text_len(value);
    int out = 0;
    for (int i = 0; i < n;) {
        if (value[i] == '&') {
            uint32_t cp;
            i = decode_entity(value, i, n, &cp);
            value[out++] = cp < 0x80 ? (char)cp : '?';
        } else {
            value[out++] = value[i++];
        }
    }
    value[out] = 0;
}

/* Parses a tag starting at s[i] == '<'. Returns the index after '>', or -1. */
static int parse_tag(const char *s, int i, int n, struct tag *tag)
{
    int p = i + 1;
    tag->closing = 0;
    tag->name[0] = 0;
    tag->href[0] = 0;
    tag->alt[0] = 0;

    if (p < n && s[p] == '/') {
        tag->closing = 1;
        p++;
    }
    if (p >= n || !is_alpha(s[p]))
        return -1;

    int length = 0;
    while (p < n && !is_space(s[p]) && s[p] != '>' && s[p] != '/') {
        if (length < TAG_NAME_MAX - 1)
            tag->name[length++] = lower(s[p]);
        p++;
    }
    tag->name[length] = 0;

    while (p < n) {
        while (p < n && (is_space(s[p]) || s[p] == '/'))
            p++;
        if (p >= n)
            return n;
        if (s[p] == '>')
            return p + 1;

        char attribute[8];
        int attribute_length = 0;
        int too_long = 0;
        while (p < n && !is_space(s[p]) && s[p] != '=' && s[p] != '>' && s[p] != '/') {
            if (attribute_length < (int)sizeof attribute - 1)
                attribute[attribute_length++] = lower(s[p]);
            else
                too_long = 1;
            p++;
        }
        attribute[attribute_length] = 0;
        if (too_long)
            attribute[0] = 0; /* Too long to be an attribute we use. */
        if (attribute_length == 0 && p < n && s[p] == '=')
            p++;

        while (p < n && is_space(s[p]))
            p++;
        if (p >= n || s[p] != '=')
            continue;
        p++;
        while (p < n && is_space(s[p]))
            p++;
        if (p >= n)
            return n;

        char *target = 0;
        int capacity = 0;
        if (text_equal(attribute, "href")) {
            target = tag->href;
            capacity = URL_MAX;
        } else if (text_equal(attribute, "alt")) {
            target = tag->alt;
            capacity = ALT_MAX;
        }

        int count = 0;
        char quote = s[p] == '"' || s[p] == '\'' ? s[p] : 0;
        if (quote)
            p++;
        while (p < n) {
            char c = s[p];
            if (quote ? c == quote : (is_space(c) || c == '>'))
                break;
            if (target && count < capacity - 1)
                target[count++] = c;
            p++;
        }
        if (target) {
            target[count] = 0;
            decode_attribute(target);
        }
        if (quote && p < n)
            p++;
    }
    return n;
}

/* Finds the end of a raw element such as </script>. */
static int skip_raw(const char *s, int i, int n, const char *name, int *content_end)
{
    int size = text_len(name);
    for (int p = i; p + size + 2 <= n; p++) {
        if (s[p] != '<' || s[p + 1] != '/')
            continue;
        int k = 0;
        while (k < size && lower(s[p + 2 + k]) == name[k])
            k++;
        if (k < size)
            continue;
        *content_end = p;
        p += size + 2;
        while (p < n && s[p] != '>')
            p++;
        return p < n ? p + 1 : n;
    }
    *content_end = n;
    return n;
}

static void capture_title(struct builder *b, const char *s, int start, int end)
{
    struct web_doc *doc = b->doc;
    int out = 0;
    int pending_space = 0;

    for (int i = start; i < end && out < WEB_TITLE_MAX - 1;) {
        uint32_t cp;
        if (s[i] == '&')
            i = decode_entity(s, i, end, &cp);
        else if ((unsigned char)s[i] >= 0x80)
            i = decode_utf8(s, i, end, &cp);
        else
            cp = (unsigned char)s[i++];

        if (cp == ' ' || cp == '\t' || cp == '\r' || cp == '\n' || cp == 0xa0) {
            pending_space = out > 0;
            continue;
        }
        if (pending_space && out < WEB_TITLE_MAX - 2)
            doc->title[out++] = ' ';
        pending_space = 0;
        doc->title[out++] = cp >= 0x20 && cp < 0x7f ? (char)cp : '?';
    }
    doc->title[out] = 0;
}

static int add_link(struct builder *b, const char *href)
{
    struct web_doc *doc = b->doc;
    struct url resolved;
    char text[URL_MAX];

    if (doc->link_count >= WEB_LINKS_MAX || !url_resolve(b->base, href, &resolved))
        return -1;
    if (resolved.scheme != URL_HTTP && resolved.scheme != URL_HTTPS &&
        resolved.scheme != URL_ABOUT)
        return -1;

    url_format(&resolved, text, sizeof text);
    int size = text_len(text) + 1;
    if (doc->strings_length + size > WEB_STRINGS_MAX)
        return -1;

    text_copy(doc->strings + doc->strings_length, size, text);
    doc->link_urls[doc->link_count] = doc->strings_length;
    doc->strings_length += size;
    return doc->link_count++;
}

static void decrement(int *counter)
{
    if (*counter > 0)
        (*counter)--;
}

static void adjust_indent(struct builder *b, int delta)
{
    b->indent += delta;
    if (b->indent < 0)
        b->indent = 0;
}

static void start_list_item(struct builder *b)
{
    add_break(b, WEB_BREAK);
    int depth = b->list_depth > 0 ? b->list_depth - 1 : 0;
    if (depth < LIST_DEPTH && b->list_depth > 0 && b->list_ordered[depth]) {
        char number[16] = "";
        text_append_u64(number, sizeof number, (uint64_t)++b->list_number[depth]);
        text_append(number, sizeof number, ".");
        text_string(b, number);
    } else {
        text_char(b, '*');
    }
    flush_word(b);
    b->space = 1;
}

static void handle_tag(struct builder *b, const struct tag *t)
{
    static const char *const BLOCKS[] = {
        "div", "section", "article", "header", "footer", "nav", "main", "aside", "form",
        "center", "table", "tbody", "thead", "tfoot", "tr", "address", "figure", "figcaption",
        "details", "summary", "dl", "fieldset", "caption", 0,
    };
    const char *name = t->name;

    flush_word(b);

    if (name[0] == 'h' && name[1] >= '1' && name[1] <= '6' && !name[2]) {
        add_break(b, WEB_GAP);
        if (t->closing) {
            decrement(&b->heading);
            if (name[1] == '1')
                decrement(&b->big);
        } else {
            b->heading++;
            if (name[1] == '1')
                b->big++;
        }
    } else if (text_equal(name, "p")) {
        add_break(b, WEB_GAP);
    } else if (text_equal(name, "br")) {
        add_break(b, WEB_NEWLINE);
    } else if (text_equal(name, "hr")) {
        add_break(b, WEB_BREAK);
        add_item(b, WEB_HR);
        add_break(b, WEB_BREAK);
    } else if (text_equal(name, "a")) {
        b->link = t->closing || !t->href[0] ? -1 : add_link(b, t->href);
    } else if (text_equal(name, "b") || text_equal(name, "strong")) {
        if (t->closing)
            decrement(&b->bold);
        else
            b->bold++;
    } else if (text_equal(name, "code") || text_equal(name, "kbd") || text_equal(name, "samp") ||
               text_equal(name, "tt")) {
        if (t->closing)
            decrement(&b->code);
        else
            b->code++;
    } else if (text_equal(name, "pre")) {
        add_break(b, WEB_GAP);
        if (t->closing)
            decrement(&b->pre);
        else
            b->pre++;
    } else if (text_equal(name, "ul") || text_equal(name, "ol") || text_equal(name, "menu")) {
        add_break(b, b->list_depth == 0 ? WEB_GAP : WEB_BREAK);
        if (t->closing) {
            if (b->list_depth > 0) {
                b->list_depth--;
                adjust_indent(b, -2);
            }
        } else {
            if (b->list_depth < LIST_DEPTH) {
                b->list_ordered[b->list_depth] = name[0] == 'o';
                b->list_number[b->list_depth] = 0;
            }
            b->list_depth++;
            adjust_indent(b, 2);
        }
    } else if (text_equal(name, "li")) {
        if (t->closing)
            add_break(b, WEB_BREAK);
        else
            start_list_item(b);
    } else if (text_equal(name, "blockquote")) {
        add_break(b, WEB_GAP);
        adjust_indent(b, t->closing ? -2 : 2);
        if (t->closing)
            decrement(&b->quote);
        else
            b->quote++;
    } else if (text_equal(name, "dd")) {
        add_break(b, WEB_BREAK);
        adjust_indent(b, t->closing ? -2 : 2);
    } else if (text_equal(name, "dt")) {
        add_break(b, WEB_BREAK);
    } else if (text_equal(name, "td") || text_equal(name, "th")) {
        b->space = 1;
    } else if (text_equal(name, "img") && !t->closing) {
        b->dim = 1;
        b->space = 1;
        text_string(b, t->alt[0] ? "[image: " : "[image]");
        if (t->alt[0]) {
            text_string(b, t->alt);
            text_char(b, ']');
        }
        flush_word(b);
        b->dim = 0;
        b->space = 1;
    } else if (one_of(name, BLOCKS)) {
        add_break(b, WEB_BREAK);
    }
}

static int handle_markup(struct builder *b, const char *s, int i, int n)
{
    static const char *const RAW[] = {
        "script", "style", "svg", "template", "iframe", "object", "canvas", "textarea", "select", 0,
    };

    if (i + 3 < n && s[i + 1] == '!' && s[i + 2] == '-' && s[i + 3] == '-') {
        for (int p = i + 4; p + 2 < n; p++) {
            if (s[p] == '-' && s[p + 1] == '-' && s[p + 2] == '>')
                return p + 3;
        }
        return n;
    }
    if (i + 1 < n && (s[i + 1] == '!' || s[i + 1] == '?')) {
        int p = i + 2;
        while (p < n && s[p] != '>')
            p++;
        return p < n ? p + 1 : n;
    }

    struct tag tag;
    int next = parse_tag(s, i, n, &tag);
    if (next < 0) {
        text_char(b, '<');
        return i + 1;
    }

    if (!tag.closing && text_equal(tag.name, "title")) {
        int content_end;
        int after = skip_raw(s, next, n, "title", &content_end);
        capture_title(b, s, next, content_end);
        return after;
    }
    if (!tag.closing && one_of(tag.name, RAW)) {
        int content_end;
        return skip_raw(s, next, n, tag.name, &content_end);
    }

    handle_tag(b, &tag);
    return next;
}

static void builder_init(struct builder *b, struct web_doc *doc, const struct url *base)
{
    b->doc = doc;
    b->base = base;
    b->bold = 0;
    b->heading = 0;
    b->code = 0;
    b->pre = 0;
    b->dim = 0;
    b->big = 0;
    b->quote = 0;
    b->link = -1;
    b->indent = 0;
    b->space = 0;
    b->word_start = -1;
    b->list_depth = 0;
}

void web_parse_html(struct web_doc *doc, const struct url *base, const char *html, int length)
{
    struct builder b;
    builder_init(&b, doc, base);

    int i = 0;
    while (i < length && doc->text_length < WEB_TEXT_MAX && doc->item_count < WEB_ITEMS_MAX) {
        char c = html[i];
        if (c == '<') {
            i = handle_markup(&b, html, i, length);
        } else if (c == '&') {
            uint32_t cp;
            i = decode_entity(html, i, length, &cp);
            put_codepoint(&b, cp);
        } else if ((unsigned char)c >= 0x80) {
            uint32_t cp;
            i = decode_utf8(html, i, length, &cp);
            put_codepoint(&b, cp);
        } else {
            if ((unsigned char)c >= 0x20 || is_space(c))
                text_char(&b, c);
            i++;
        }
    }
    if (i < length)
        doc->truncated = 1;
    flush_word(&b);
}

void web_parse_text(struct web_doc *doc, const char *text, int length)
{
    struct builder b;
    builder_init(&b, doc, 0);
    b.pre = 1;

    int i = 0;
    while (i < length && doc->text_length < WEB_TEXT_MAX && doc->item_count < WEB_ITEMS_MAX) {
        if ((unsigned char)text[i] >= 0x80) {
            uint32_t cp;
            i = decode_utf8(text, i, length, &cp);
            put_codepoint(&b, cp);
        } else {
            char c = text[i++];
            if ((unsigned char)c >= 0x20 || is_space(c))
                text_char(&b, c);
        }
    }
    if (i < length)
        doc->truncated = 1;
    flush_word(&b);
}

static int add_run(struct web_doc *doc, int line, int column, const struct web_item *item,
                   uint32_t text, int length)
{
    if (doc->run_count >= WEB_RUNS_MAX) {
        doc->truncated = 1;
        return 0;
    }
    struct web_run *run = &doc->runs[doc->run_count++];
    run->line = line;
    run->text = text;
    run->column = (uint16_t)column;
    run->length = (uint16_t)length;
    run->link = item->link;
    run->style = item->style;
    return 1;
}

void web_layout(struct web_doc *doc, int columns)
{
    if (columns < 8)
        columns = 8;
    doc->columns = columns;
    doc->run_count = 0;

    int line = 0;
    int height = 1; /* Lines used by the current line, 2 if it holds a big heading. */
    int column = 0;
    int line_has_text = 0;
    int previous_blank = 1;

#define NEXT_LINE()          \
    do {                     \
        line += height;      \
        height = 1;          \
        column = 0;          \
        line_has_text = 0;   \
    } while (0)

    for (int i = 0; i < doc->item_count; i++) {
        const struct web_item *item = &doc->items[i];
        int indent = item->indent > columns / 2 ? columns / 2 : item->indent;

        if (item->kind == WEB_WORD) {
            int scale = (item->style & WEB_BIG) ? 2 : 1;
            uint32_t text = item->text;
            int remaining = item->length;
            int gap = item->space && line_has_text;

            if (!line_has_text)
                column = indent;
            if (line_has_text && column + (gap + remaining) * scale > columns) {
                NEXT_LINE();
                column = indent;
                gap = 0;
            }
            column += gap * scale;

            while (remaining > 0) {
                int room = (columns - column) / scale;
                if (room <= 0) {
                    NEXT_LINE();
                    column = indent;
                    room = (columns - column) / scale;
                    if (room <= 0)
                        room = 1;
                }
                int count = remaining < room ? remaining : room;
                if (!add_run(doc, line, column, item, text, count))
                    goto done;
                if (scale > height)
                    height = scale;
                text += (uint32_t)count;
                remaining -= count;
                column += count * scale;
                line_has_text = 1;
                previous_blank = 0;
            }
        } else if (item->kind == WEB_BREAK) {
            if (line_has_text)
                NEXT_LINE();
        } else if (item->kind == WEB_NEWLINE) {
            if (line_has_text) {
                NEXT_LINE();
            } else if (!previous_blank) {
                line++;
                previous_blank = 1;
            }
        } else if (item->kind == WEB_GAP) {
            if (line_has_text)
                NEXT_LINE();
            if (!previous_blank) {
                line++;
                previous_blank = 1;
            }
        } else if (item->kind == WEB_HR) {
            struct web_item rule = *item;
            rule.style = WEB_RULE;
            rule.link = -1;
            if (line_has_text)
                NEXT_LINE();
            if (!add_run(doc, line, 0, &rule, 0, columns))
                goto done;
            line++;
            previous_blank = 0;
        }
    }
#undef NEXT_LINE

done:
    doc->line_count = line + (line_has_text ? height : 0);
}
