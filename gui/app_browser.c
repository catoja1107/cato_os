/* The Web Browser app: tabs, bookmarks, HTTP and HTTPS, and a readable page view. */
#include "apps.h"
#include "html.h"
#include "http.h"
#include "net.h"
#include "shell.h"
#include "text.h"

#define TAB_STRIP_H 30
#define NAV_H 38
#define BOOKMARK_BAR_H 26
#define FIND_H 30
#define STATUS_H 22
#define SCROLLBAR_W 10
#define PAGE_PADDING 14
#define MAX_COLUMNS 96
#define TAB_MAX_W 200
#define BUTTON_W 30
#define MAX_REDIRECTS 5
#define RUN_DRAW_MAX 255
#define WHEEL_LINES 3
#define BOOKMARKS_NAME "Bookmarks.txt"
#define SEARCH_PREFIX "https://lite.duckduckgo.com/lite/?q="

#define COLOR_PAGE RGB(30, 30, 37)
#define COLOR_CHROME RGB(22, 22, 28)
#define COLOR_FIELD RGB(14, 14, 18)
#define COLOR_LINK RGB(122, 182, 255)
#define COLOR_LINK_HOVER RGB(175, 212, 255)
#define COLOR_CODE RGB(150, 214, 150)
#define COLOR_CODE_BG RGB(20, 25, 22)
#define COLOR_BOLD RGB(255, 255, 255)
#define COLOR_SECURE RGB(110, 200, 120)
#define COLOR_INSECURE RGB(232, 110, 90)
#define COLOR_MATCH RGB(120, 88, 28)

static const char *const ICON_BACK[7] = {
    "   #   ", "  #    ", " #     ", "#######", " #     ", "  #    ", "   #   ",
};
static const char *const ICON_FORWARD[7] = {
    "   #   ", "    #  ", "     # ", "#######", "     # ", "    #  ", "   #   ",
};
static const char *const ICON_RELOAD[7] = {
    "  #### ", " #    #", "#    ##", "#   ###", "#      ", " #    #", "  #### ",
};
static const char *const ICON_STOP[7] = {
    "#     #", " #   # ", "  # #  ", "   #   ", "  # #  ", " #   # ", "#     #",
};
static const char *const ICON_HOME[7] = {
    "   #   ", "  ###  ", " ##### ", "#######", " #   # ", " # # # ", " # # # ",
};
static const char *const ICON_STAR[7] = {
    "   #   ", "  ###  ", "#######", " ##### ", "  ###  ", " ## ## ", " #   # ",
};
static const char *const ICON_LOCK[7] = {
    " ### ", "#   #", "#   #", "#####", "## ##", "## ##", "#####",
};
static const char *const ICON_WARNING[7] = {
    "  #  ", " ### ", " ### ", " ### ", "  #  ", "     ", "  #  ",
};
static const char *const ICON_INFO[7] = {
    "  #  ", "     ", " ##  ", "  #  ", "  #  ", "  #  ", " ### ",
};

static const char *const DEFAULT_BOOKMARKS[][2] = {
    { "https://news.ycombinator.com/", "Hacker News" },
    { "https://en.wikipedia.org/", "Wikipedia" },
    { "https://lite.duckduckgo.com/lite/", "DuckDuckGo" },
    { "http://68k.news/", "68k.news" },
    { "https://example.com/", "example.com" },
};

/* These are large, so they live in static memory. Each tab has its own page. */
static struct web_doc g_docs[BROWSER_TABS];
static struct http_client g_http;
static uint8_t g_body[HTTP_BODY_MAX];
static int g_page_length;

struct browser_layout {
    struct rect tabs;
    struct rect nav;
    struct rect back;
    struct rect forward;
    struct rect reload;
    struct rect home;
    struct rect address;
    struct rect star;
    struct rect bookmarks;
    struct rect page;
    struct rect scrollbar;
    struct rect find;
    struct rect status;
    int rows;
    int columns;
    int text_x;
};

static struct browser_layout compute_layout(const struct wm *wm, struct rect c)
{
    struct browser_layout l;
    int y = c.y;

    l.tabs = (struct rect){ c.x, y, c.w, TAB_STRIP_H };
    y += TAB_STRIP_H;
    l.nav = (struct rect){ c.x, y, c.w, NAV_H };
    l.back = (struct rect){ c.x + 8, y + 5, BUTTON_W, 28 };
    l.forward = (struct rect){ l.back.x + BUTTON_W + 4, y + 5, BUTTON_W, 28 };
    l.reload = (struct rect){ l.forward.x + BUTTON_W + 4, y + 5, BUTTON_W, 28 };
    l.home = (struct rect){ l.reload.x + BUTTON_W + 4, y + 5, BUTTON_W, 28 };
    l.star = (struct rect){ c.x + c.w - BUTTON_W - 8, y + 5, BUTTON_W, 28 };
    int address_x = l.home.x + BUTTON_W + 8;
    l.address = (struct rect){ address_x, y + 5, l.star.x - 6 - address_x, 28 };
    y += NAV_H;
    l.bookmarks = (struct rect){ c.x, y, c.w, BOOKMARK_BAR_H };
    y += BOOKMARK_BAR_H;

    int bottom = c.y + c.h - STATUS_H;
    l.status = (struct rect){ c.x, bottom, c.w, STATUS_H };
    if (wm->browser.finding) {
        bottom -= FIND_H;
        l.find = (struct rect){ c.x, bottom, c.w, FIND_H };
    } else {
        l.find = (struct rect){ 0, 0, 0, 0 };
    }

    l.page = (struct rect){ c.x, y, c.w - SCROLLBAR_W, bottom - y };
    l.scrollbar = (struct rect){ c.x + c.w - SCROLLBAR_W, y, SCROLLBAR_W, bottom - y };
    l.columns = (l.page.w - PAGE_PADDING * 2) / FONT_ADVANCE;
    if (l.columns > MAX_COLUMNS)
        l.columns = MAX_COLUMNS;
    if (l.columns < 8)
        l.columns = 8;
    l.text_x = l.page.x + (l.page.w - l.columns * FONT_ADVANCE) / 2;
    l.rows = (l.page.h - PAGE_PADDING * 2) / FONT_LINE;
    if (l.rows < 1)
        l.rows = 1;
    return l;
}

static struct browser_layout current_layout(const struct wm *wm)
{
    return compute_layout(wm, wm_content_rect(wm, APP_BROWSER));
}

static struct browser_tab *tab_at(struct wm *wm, int index)
{
    return &wm->browser.tabs[index];
}

static struct browser_tab *active_tab(struct wm *wm)
{
    return tab_at(wm, wm->browser.active);
}

static int tab_count(const struct wm *wm)
{
    int count = 0;
    for (int i = 0; i < BROWSER_TABS; i++)
        count += wm->browser.tabs[i].used;
    return count;
}

static void invalidate(struct wm *wm)
{
    wm_invalidate_app(wm, APP_BROWSER);
}

static void set_status(struct wm *wm, int tab, const char *text)
{
    text_copy(tab_at(wm, tab)->status, sizeof tab_at(wm, tab)->status, text);
    if (tab == wm->browser.active)
        invalidate(wm);
}

static int layout_columns(const struct wm *wm)
{
    return wm->windows[APP_BROWSER].open ? current_layout(wm).columns : 80;
}

static void clamp_scroll(struct wm *wm)
{
    struct browser_tab *tab = active_tab(wm);
    int rows = wm->windows[APP_BROWSER].open ? current_layout(wm).rows : 1;
    int max = g_docs[wm->browser.active].line_count - rows;
    if (tab->top_line > max)
        tab->top_line = max;
    if (tab->top_line < 0)
        tab->top_line = 0;
}

static void ensure_layout(struct wm *wm)
{
    struct web_doc *doc = &g_docs[wm->browser.active];
    int columns = layout_columns(wm);
    if (doc->columns != columns) {
        web_layout(doc, columns);
        clamp_scroll(wm);
    }
}

/* Built-in pages */

static void page_reset(void)
{
    g_page_length = 0;
}

static void page_add(const char *text)
{
    while (*text && g_page_length < HTTP_BODY_MAX)
        g_body[g_page_length++] = (uint8_t)*text++;
}

static void page_add_escaped(const char *text)
{
    for (; *text; text++) {
        if (*text == '<')
            page_add("&lt;");
        else if (*text == '>')
            page_add("&gt;");
        else if (*text == '&')
            page_add("&amp;");
        else if (*text == '"')
            page_add("&quot;");
        else if (g_page_length < HTTP_BODY_MAX)
            g_body[g_page_length++] = (uint8_t)*text;
    }
}

static void adopt_title(struct wm *wm, int tab)
{
    struct browser_tab *t = tab_at(wm, tab);
    text_copy(t->title, sizeof t->title, g_docs[tab].title);
}

static void render_built_page(struct wm *wm, int tab, const char *base_text)
{
    struct url base;
    struct browser_tab *t = tab_at(wm, tab);
    url_parse(base_text, &base);
    web_doc_clear(&g_docs[tab]);
    web_parse_html(&g_docs[tab], &base, (const char *)g_body, g_page_length);
    web_layout(&g_docs[tab], layout_columns(wm));
    t->top_line = 0;
    t->link_index = -1;
    t->security = SECURITY_INTERNAL;
    adopt_title(wm, tab);
    if (tab == wm->browser.active) {
        wm->browser.find_run = -1;
        invalidate(wm);
    }
}

static void show_message(struct wm *wm, int tab, const char *heading, const char *message,
                         const char *address)
{
    page_reset();
    page_add("<title>");
    page_add_escaped(heading);
    page_add("</title><h1>");
    page_add_escaped(heading);
    page_add("</h1><p>");
    page_add_escaped(message);
    page_add("</p>");
    if (address && address[0]) {
        page_add("<p>Address: <code>");
        page_add_escaped(address);
        page_add("</code></p>");
    }
    page_add("<p><a href=\"about:home\">Go to the start page</a></p>");
    render_built_page(wm, tab, "about:message");
    set_status(wm, tab, heading);
}

static void show_home(struct wm *wm, int tab)
{
    const struct browser_app *b = &wm->browser;
    page_reset();
    page_add("<title>Start</title><h1>Cato Browser</h1>"
             "<p>Type an address or a search in the bar above and press Enter.</p>"
             "<h2>Bookmarks</h2><ul>");
    for (int i = 0; i < b->bookmark_count; i++) {
        page_add("<li><a href=\"");
        page_add_escaped(b->bookmark_url[i]);
        page_add("\">");
        page_add_escaped(b->bookmark_title[i]);
        page_add("</a> <code>");
        page_add_escaped(b->bookmark_url[i]);
        page_add("</code></li>");
    }
    if (b->bookmark_count == 0)
        page_add("<li>No bookmarks yet. Press Ctrl+D to add one.</li>");
    page_add("<li><a href=\"about:network\">Network status</a></li></ul>"
             "<h2>About this browser</h2>"
             "<p>Pages are shown as text and links. Secure sites use HTTPS with "
             "TLS 1.2, and certificates are checked against 120 trusted authorities. "
             "JavaScript, CSS, images, and forms are not supported, so many modern "
             "sites will look plain or not work.</p>"
             "<h2>Keys</h2><ul>"
             "<li>Type to edit the address. Text that is not an address is searched.</li>"
             "<li>Ctrl+T opens a tab, Ctrl+W closes it, and F7 and F8 switch tabs.</li>"
             "<li>Ctrl+F finds text, and Ctrl+D bookmarks the page.</li>"
             "<li>Left and Right pick a link, and Enter opens it.</li>"
             "<li>Backspace goes back, F5 reloads, and Esc stops loading.</li>"
             "<li>Right-click a link to open it in a new tab.</li>"
             "</ul>");
    render_built_page(wm, tab, "about:home");
    set_status(wm, tab, "Ready");
}

static void show_network(struct wm *wm, int tab)
{
    char value[24];
    page_reset();
    page_add("<title>Network status</title><h1>Network status</h1>");
    if (!wm->net) {
        page_add("<p>This system has no network support.</p>");
    } else {
        const struct net *net = wm->net;
        page_add("<p><b>State:</b> ");
        page_add_escaped(net_state_text(net));
        page_add("</p>");
        if (net->state == NET_READY) {
            static const char *const LABELS[4] = { "Address", "Subnet mask", "Router", "DNS server" };
            const uint32_t values[4] = { net->ip, net->mask, net->router, net->dns_server };
            page_add("<ul>");
            for (int i = 0; i < 4; i++) {
                net_format_ip(values[i], value, sizeof value);
                page_add("<li><b>");
                page_add(LABELS[i]);
                page_add(":</b> ");
                page_add(value);
                page_add("</li>");
            }
            page_add("</ul>");
        }
    }
    page_add("<p><a href=\"about:home\">Go to the start page</a></p>");
    render_built_page(wm, tab, "about:network");
    set_status(wm, tab, "Ready");
}

/* Bookmarks are kept in a file, so they are saved with everything else. */

static int bookmarks_file(const struct wm *wm)
{
    return fs_lookup(wm->fs, wm->fs->documents, BOOKMARKS_NAME);
}

static void add_bookmark_entry(struct browser_app *b, const char *url, const char *title)
{
    if (b->bookmark_count >= BOOKMARKS_MAX || !url[0])
        return;
    text_copy(b->bookmark_url[b->bookmark_count], URL_MAX, url);
    text_copy(b->bookmark_title[b->bookmark_count], BOOKMARK_TITLE_MAX, title[0] ? title : url);
    b->bookmark_count++;
}

static void bookmarks_load(struct wm *wm)
{
    static char buffer[BOOKMARKS_MAX * (URL_MAX + BOOKMARK_TITLE_MAX + 2)];
    struct browser_app *b = &wm->browser;
    b->bookmark_count = 0;

    int node = bookmarks_file(wm);
    if (!fs_is_file(wm->fs, node)) {
        for (unsigned i = 0; i < sizeof DEFAULT_BOOKMARKS / sizeof DEFAULT_BOOKMARKS[0]; i++)
            add_bookmark_entry(b, DEFAULT_BOOKMARKS[i][0], DEFAULT_BOOKMARKS[i][1]);
        return;
    }

    int length = fs_read(wm->fs, node, 0, buffer, sizeof buffer - 1);
    buffer[length > 0 ? length : 0] = 0;
    char *line = buffer;
    while (*line) {
        char *end = line;
        while (*end && *end != '\n')
            end++;
        char saved = *end;
        *end = 0;

        char *tab = line;
        while (*tab && *tab != '\t')
            tab++;
        if (*tab) {
            *tab = 0;
            struct url check;
            if (url_parse(line, &check))
                add_bookmark_entry(b, line, tab + 1);
        }
        if (!saved)
            break;
        line = end + 1;
    }
}

static void bookmarks_save(struct wm *wm)
{
    static char buffer[BOOKMARKS_MAX * (URL_MAX + BOOKMARK_TITLE_MAX + 2)];
    const struct browser_app *b = &wm->browser;

    buffer[0] = 0;
    for (int i = 0; i < b->bookmark_count; i++) {
        text_append(buffer, sizeof buffer, b->bookmark_url[i]);
        text_append(buffer, sizeof buffer, "\t");
        text_append(buffer, sizeof buffer, b->bookmark_title[i]);
        text_append(buffer, sizeof buffer, "\n");
    }

    int node = bookmarks_file(wm);
    if (!fs_is_file(wm->fs, node))
        node = fs_create(wm->fs, wm->fs->documents, BOOKMARKS_NAME, FS_FILE);
    if (node < 0 || fs_write(wm->fs, node, buffer, text_len(buffer)) != FS_OK) {
        shell_error(wm, node < 0 ? node : FS_ERR_NO_SPACE);
        return;
    }
    shell_fs_changed(wm);
}

static int bookmark_index(const struct wm *wm, const char *url)
{
    for (int i = 0; i < wm->browser.bookmark_count; i++) {
        if (text_equal(wm->browser.bookmark_url[i], url))
            return i;
    }
    return -1;
}

static void toggle_bookmark(struct wm *wm)
{
    struct browser_app *b = &wm->browser;
    struct browser_tab *tab = active_tab(wm);
    if (!tab->address[0])
        return;

    int index = bookmark_index(wm, tab->address);
    if (index >= 0) {
        for (int i = index + 1; i < b->bookmark_count; i++) {
            text_copy(b->bookmark_url[i - 1], URL_MAX, b->bookmark_url[i]);
            text_copy(b->bookmark_title[i - 1], BOOKMARK_TITLE_MAX, b->bookmark_title[i]);
        }
        b->bookmark_count--;
        set_status(wm, b->active, "Bookmark removed");
    } else if (b->bookmark_count < BOOKMARKS_MAX) {
        add_bookmark_entry(b, tab->address, tab->title);
        set_status(wm, b->active, "Bookmarked");
    } else {
        set_status(wm, b->active, "The bookmark list is full");
        return;
    }
    wm_trace(wm, "web:bookmarks-changed");
    bookmarks_save(wm);
    invalidate(wm);
}

/* Loading */

static void cancel_tab(struct wm *wm, int tab)
{
    struct browser_app *b = &wm->browser;
    if (b->loading_tab == tab) {
        http_cancel(&g_http);
        b->loading_tab = -1;
        b->fetching = 0;
    }
    tab_at(wm, tab)->state = TAB_IDLE;
}

static void load_tab(struct wm *wm, int tab, const struct url *url)
{
    struct browser_tab *t = tab_at(wm, tab);
    char text[URL_MAX];
    url_format(url, text, sizeof text);

    cancel_tab(wm, tab);
    switch (url->scheme) {
    case URL_ABOUT:
        if (text_equal(url->path, "home"))
            show_home(wm, tab);
        else if (text_equal(url->path, "network"))
            show_network(wm, tab);
        else
            show_message(wm, tab, "Page not found", "That built-in page does not exist.", text);
        break;
    case URL_HTTP:
    case URL_HTTPS:
        if (!wm->net) {
            show_message(wm, tab, "No network", "This system has no network support.", text);
            break;
        }
        terminal_cancel_ping(wm);
        t->target = *url;
        t->state = TAB_QUEUED;
        net_start(wm->net);
        set_status(wm, tab, "Waiting for the network...");
        break;
    default:
        show_message(wm, tab, "Unsupported link",
                     "This browser can only open http and https addresses.", text);
        break;
    }
}

static void push_history(struct browser_tab *t, const char *text)
{
    if (t->history_count > 0 && text_equal(t->history[t->history_pos], text))
        return;

    int next = t->history_count == 0 ? 0 : t->history_pos + 1;
    if (next == BROWSER_HISTORY) {
        for (int i = 1; i < BROWSER_HISTORY; i++)
            text_copy(t->history[i - 1], URL_MAX, t->history[i]);
        next = BROWSER_HISTORY - 1;
    }
    text_copy(t->history[next], URL_MAX, text);
    t->history_pos = next;
    t->history_count = next + 1;
}

static void navigate_tab(struct wm *wm, int tab, const char *text)
{
    struct browser_tab *t = tab_at(wm, tab);
    struct url url;
    char formatted[URL_MAX];

    if (!url_parse(text, &url)) {
        cancel_tab(wm, tab);
        show_message(wm, tab, "Invalid address", "That address could not be understood.", text);
        return;
    }
    url_format(&url, formatted, sizeof formatted);
    if (url.scheme == URL_OTHER)
        text_copy(formatted, sizeof formatted, text);

    t->redirects = 0;
    text_copy(t->address, sizeof t->address, formatted);
    push_history(t, formatted);
    load_tab(wm, tab, &url);
}

void browser_navigate(struct wm *wm, const char *text)
{
    wm->browser.editing = 0;
    navigate_tab(wm, wm->browser.active, text);
}

static void go_history(struct wm *wm, int delta)
{
    struct browser_tab *t = active_tab(wm);
    int position = t->history_pos + delta;
    struct url url;

    if (position < 0 || position >= t->history_count)
        return;
    t->history_pos = position;
    t->redirects = 0;
    text_copy(t->address, sizeof t->address, t->history[position]);
    if (url_parse(t->history[position], &url))
        load_tab(wm, wm->browser.active, &url);
}

static void reload(struct wm *wm)
{
    struct browser_tab *t = active_tab(wm);
    struct url url;
    if (t->history_count > 0 && url_parse(t->history[t->history_pos], &url)) {
        t->redirects = 0;
        load_tab(wm, wm->browser.active, &url);
    }
}

static int contains(const char *text, const char *word)
{
    int size = text_len(word);
    for (int i = 0; text[i]; i++) {
        int k = 0;
        while (k < size && text[i + k] == word[k])
            k++;
        if (k == size)
            return 1;
    }
    return 0;
}

static void trace_event(const struct wm *wm, const char *verb, const char *detail)
{
    char event[160];
    text_copy(event, sizeof event, verb);
    text_append(event, sizeof event, detail);
    wm_trace(wm, event);
}

static void load_failed(struct wm *wm, int tab, const char *heading, const char *message)
{
    char text[URL_MAX];
    url_format(&tab_at(wm, tab)->target, text, sizeof text);
    trace_event(wm, "web:error:", message);
    show_message(wm, tab, heading, message, text);
}

static void finish_load(struct wm *wm, int tab)
{
    struct browser_app *b = &wm->browser;
    struct browser_tab *t = tab_at(wm, tab);
    b->loading_tab = -1;
    b->fetching = 0;
    t->state = TAB_IDLE;

    int status = g_http.status;
    if ((status == 301 || status == 302 || status == 303 || status == 307 || status == 308) &&
        g_http.location[0]) {
        struct url next;
        if (++t->redirects > MAX_REDIRECTS) {
            load_failed(wm, tab, "Too many redirects", "This site keeps redirecting.");
        } else if (!url_resolve(&t->target, g_http.location, &next)) {
            load_failed(wm, tab, "Bad redirect", "The site sent an invalid address.");
        } else {
            int redirects = t->redirects;
            url_format(&next, t->address, sizeof t->address);
            if (t->history_count > 0)
                text_copy(t->history[t->history_pos], URL_MAX, t->address);
            load_tab(wm, tab, &next);
            t->redirects = redirects;
        }
        return;
    }

    const char *type = g_http.content_type;
    int is_html = type[0] == 0 || contains(type, "html");
    int is_text = contains(type, "text/");
    if (!is_html && !is_text) {
        load_failed(wm, tab, "Cannot display this file",
                    "This browser can only show web pages and plain text.");
        return;
    }
    if (status >= 400 && g_http.body_length == 0) {
        char message[48] = "The server returned error ";
        text_append_u64(message, sizeof message, (uint64_t)status);
        text_append(message, sizeof message, ".");
        load_failed(wm, tab, "Page error", message);
        return;
    }

    struct web_doc *doc = &g_docs[tab];
    web_doc_clear(doc);
    if (is_html)
        web_parse_html(doc, &t->target, (const char *)g_body, g_http.body_length);
    else
        web_parse_text(doc, (const char *)g_body, g_http.body_length);
    web_layout(doc, layout_columns(wm));
    t->top_line = 0;
    t->link_index = -1;
    t->security = g_http.secure ? SECURITY_VERIFIED : SECURITY_PLAIN;
    t->tls_version = g_http.tls_version;
    adopt_title(wm, tab);
    if (!t->title[0])
        text_copy(t->title, sizeof t->title, t->target.host);
    if (tab == b->active)
        b->find_run = -1;

    if (g_http.truncated || doc->truncated)
        set_status(wm, tab, "Done, but the page was too large or ended early");
    else
        set_status(wm, tab, status >= 400 ? "Done (the server reported an error)" : "Done");

    char event[128];
    text_copy(event, sizeof event, "web:loaded:");
    text_append_u64(event, sizeof event, (uint64_t)status);
    text_append(event, sizeof event, ":");
    text_append_u64(event, sizeof event, (uint64_t)g_http.body_length);
    text_append(event, sizeof event, ":");
    text_append(event, sizeof event, doc->title);
    wm_trace(wm, event);
    if (g_http.secure)
        trace_event(wm, "web:secure:", t->target.host);
    invalidate(wm);
}

static void start_queued(struct wm *wm)
{
    struct browser_app *b = &wm->browser;
    struct net *net = wm->net;

    for (int tab = 0; tab < BROWSER_TABS; tab++) {
        struct browser_tab *t = tab_at(wm, tab);
        if (!t->used || t->state != TAB_QUEUED)
            continue;
        if (net->state == NET_FAILED) {
            t->state = TAB_IDLE;
            load_failed(wm, tab, "No network connection", net_state_text(net));
            continue;
        }
        if (net->state != NET_READY)
            return;
        http_start(&g_http, net, &t->target, g_body, HTTP_BODY_MAX);
        t->state = TAB_LOADING;
        b->loading_tab = tab;
        b->fetching = 1;
        return;
    }
}

void browser_tick(struct wm *wm)
{
    struct browser_app *b = &wm->browser;
    if (b->loading_tab < 0)
        start_queued(wm);
    if (b->loading_tab < 0)
        return;

    int tab = b->loading_tab;
    int before = g_http.phase;
    http_poll(&g_http);

    if (g_http.phase == HTTP_DONE) {
        finish_load(wm, tab);
    } else if (g_http.phase == HTTP_FAILED) {
        char error[sizeof g_http.error];
        text_copy(error, sizeof error, g_http.error);
        b->loading_tab = -1;
        b->fetching = 0;
        tab_at(wm, tab)->state = TAB_IDLE;
        load_failed(wm, tab, g_http.secure ? "Secure connection failed" : "Could not load the page",
                    error);
    } else if (g_http.phase != before || g_http.phase == HTTP_BODY) {
        char status[64];
        switch (g_http.phase) {
        case HTTP_RESOLVING:
            text_copy(status, sizeof status, "Looking up the site...");
            break;
        case HTTP_CONNECTING:
            text_copy(status, sizeof status, "Connecting...");
            break;
        case HTTP_HANDSHAKE:
            text_copy(status, sizeof status, "Securing the connection...");
            break;
        case HTTP_HEADERS:
            text_copy(status, sizeof status, "Waiting for the server...");
            break;
        default:
            text_copy(status, sizeof status, "Loading: ");
            text_append_u64(status, sizeof status, (uint64_t)g_http.body_length / 1024);
            text_append(status, sizeof status, " KB");
            break;
        }
        if (!text_equal(status, tab_at(wm, tab)->status))
            set_status(wm, tab, status);
    }

    /* Keep the progress bar moving. */
    if (tab == b->active && wm->windows[APP_BROWSER].open) {
        struct browser_layout l = current_layout(wm);
        wm_invalidate(wm, (struct rect){ l.page.x, l.page.y, l.page.w, 3 });
    }
}

/* Tabs */

static void reset_tab(struct browser_tab *t)
{
    t->state = TAB_IDLE;
    t->security = SECURITY_INTERNAL;
    t->tls_version = 0;
    t->redirects = 0;
    t->top_line = 0;
    t->link_index = -1;
    t->address[0] = 0;
    t->title[0] = 0;
    t->status[0] = 0;
    t->history_count = 0;
    t->history_pos = 0;
}

static void activate_tab(struct wm *wm, int tab)
{
    struct browser_app *b = &wm->browser;
    if (!tab_at(wm, tab)->used)
        return;
    b->active = tab;
    b->editing = 0;
    b->find_run = -1;
    b->hover_link = -1;
    ensure_layout(wm);
    char event[24] = "web:tab:";
    text_append_u64(event, sizeof event, (uint64_t)tab);
    wm_trace(wm, event);
    invalidate(wm);
}

static void begin_edit(struct wm *wm, int select_all);

static int new_tab(struct wm *wm, const char *address)
{
    for (int tab = 0; tab < BROWSER_TABS; tab++) {
        struct browser_tab *t = tab_at(wm, tab);
        if (t->used)
            continue;
        reset_tab(t);
        t->used = 1;
        navigate_tab(wm, tab, address);
        activate_tab(wm, tab);
        return tab;
    }
    set_status(wm, wm->browser.active, "All tabs are in use. Close one with Ctrl+W.");
    return -1;
}

static void close_tab(struct wm *wm, int tab)
{
    struct browser_app *b = &wm->browser;
    cancel_tab(wm, tab);
    if (tab_count(wm) <= 1) {
        reset_tab(tab_at(wm, tab));
        navigate_tab(wm, tab, "about:home");
        activate_tab(wm, tab);
        return;
    }
    tab_at(wm, tab)->used = 0;
    if (b->active == tab) {
        int next = tab;
        for (int step = 1; step < BROWSER_TABS; step++) {
            int candidate = (tab + BROWSER_TABS - step) % BROWSER_TABS;
            if (tab_at(wm, candidate)->used) {
                next = candidate;
                break;
            }
        }
        activate_tab(wm, next);
    }
    invalidate(wm);
}

static void cycle_tab(struct wm *wm, int delta)
{
    int tab = wm->browser.active;
    for (int step = 0; step < BROWSER_TABS; step++) {
        tab = (tab + BROWSER_TABS + delta) % BROWSER_TABS;
        if (tab_at(wm, tab)->used) {
            activate_tab(wm, tab);
            return;
        }
    }
}

void browser_init(struct wm *wm)
{
    struct browser_app *b = &wm->browser;
    for (int tab = 0; tab < BROWSER_TABS; tab++) {
        reset_tab(&b->tabs[tab]);
        b->tabs[tab].used = 0;
    }
    b->active = 0;
    b->loading_tab = -1;
    b->fetching = 0;
    b->editing = 0;
    b->edit[0] = 0;
    b->edit_len = 0;
    b->edit_fresh = 0;
    b->finding = 0;
    b->find[0] = 0;
    b->find_len = 0;
    b->find_run = -1;
    b->hover_link = -1;
    g_http.phase = HTTP_IDLE;

    bookmarks_load(wm);
    b->tabs[0].used = 1;
    navigate_tab(wm, 0, "about:home");
}

const char *browser_page_title(const struct wm *wm)
{
    return wm->browser.tabs[wm->browser.active].title;
}

void browser_resized(struct wm *wm)
{
    ensure_layout(wm);
    clamp_scroll(wm);
}

/* Address entry */

static int is_url_character(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           c == '-' || c == '_' || c == '.' || c == '~';
}

void browser_address_for_input(const char *input, char *out, int capacity)
{
    static const char HEX[] = "0123456789ABCDEF";
    while (*input == ' ')
        input++;
    int length = text_len(input);
    while (length > 0 && input[length - 1] == ' ')
        length--;

    int has_space = 0, has_dot = 0, has_colon = 0;
    for (int i = 0; i < length; i++) {
        has_space |= input[i] == ' ';
        has_dot |= input[i] == '.';
        has_colon |= input[i] == ':';
    }
    int is_localhost = text_starts(input, "localhost");
    if (!has_space && (has_dot || has_colon || is_localhost)) {
        int count = length < capacity - 1 ? length : capacity - 1;
        for (int i = 0; i < count; i++)
            out[i] = input[i];
        out[count] = 0;
        return;
    }

    text_copy(out, capacity, SEARCH_PREFIX);
    int o = text_len(out);
    for (int i = 0; i < length && o + 4 < capacity; i++) {
        char c = input[i];
        if (c == ' ') {
            out[o++] = '+';
        } else if (is_url_character(c)) {
            out[o++] = c;
        } else {
            out[o++] = '%';
            out[o++] = HEX[(unsigned char)c >> 4];
            out[o++] = HEX[(unsigned char)c & 15];
        }
    }
    out[o] = 0;
}

static void begin_edit(struct wm *wm, int select_all)
{
    struct browser_app *b = &wm->browser;
    text_copy(b->edit, sizeof b->edit, active_tab(wm)->address);
    b->edit_len = text_len(b->edit);
    b->editing = 1;
    b->edit_fresh = select_all;
    invalidate(wm);
}

static void edit_key(struct wm *wm, int key)
{
    struct browser_app *b = &wm->browser;

    if (key == KEY_ENTER) {
        char address[URL_MAX];
        browser_address_for_input(b->edit, address, sizeof address);
        b->editing = 0;
        if (address[0])
            navigate_tab(wm, b->active, address);
        invalidate(wm);
    } else if (key == KEY_ESC) {
        b->editing = 0;
        invalidate(wm);
    } else if (key == KEY_BACKSPACE) {
        if (b->edit_fresh) {
            b->edit[0] = 0;
            b->edit_len = 0;
            b->edit_fresh = 0;
        } else if (b->edit_len > 0) {
            b->edit[--b->edit_len] = 0;
        }
        invalidate(wm);
    } else if (key >= 0x20 && key <= 0x7e) {
        if (b->edit_fresh) {
            b->edit[0] = 0;
            b->edit_len = 0;
            b->edit_fresh = 0;
        }
        if (b->edit_len < URL_MAX - 1) {
            b->edit[b->edit_len++] = (char)key;
            b->edit[b->edit_len] = 0;
        }
        invalidate(wm);
    }
}

/* Find in page */

static char lower(char c)
{
    return c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c;
}

static int run_matches(const struct web_doc *doc, const struct web_run *run, const char *needle,
                       int length)
{
    if (run->style & WEB_RULE)
        return 0;
    for (int start = 0; start + length <= run->length; start++) {
        int k = 0;
        while (k < length && lower(doc->text[run->text + (uint32_t)(start + k)]) == lower(needle[k]))
            k++;
        if (k == length)
            return 1;
    }
    return 0;
}

static void find_next(struct wm *wm)
{
    struct browser_app *b = &wm->browser;
    struct web_doc *doc = &g_docs[b->active];
    struct browser_tab *tab = active_tab(wm);
    ensure_layout(wm);
    if (b->find_len == 0 || doc->run_count == 0)
        return;

    int start = b->find_run + 1;
    for (int step = 0; step < doc->run_count; step++) {
        int i = (start + step) % doc->run_count;
        if (!run_matches(doc, &doc->runs[i], b->find, b->find_len))
            continue;
        b->find_run = i;
        int rows = wm->windows[APP_BROWSER].open ? current_layout(wm).rows : 1;
        int line = doc->runs[i].line;
        if (line < tab->top_line || line >= tab->top_line + rows)
            tab->top_line = line - rows / 3;
        clamp_scroll(wm);
        wm_trace(wm, "web:found");
        invalidate(wm);
        return;
    }
    b->find_run = -1;
    set_status(wm, b->active, "No matches on this page");
}

static void find_key(struct wm *wm, int key)
{
    struct browser_app *b = &wm->browser;
    if (key == KEY_ENTER) {
        find_next(wm);
    } else if (key == KEY_ESC || key == KEY_CTRL_F) {
        b->finding = 0;
        b->find_run = -1;
        browser_resized(wm);
        invalidate(wm);
    } else if (key == KEY_BACKSPACE) {
        if (b->find_len > 0)
            b->find[--b->find_len] = 0;
        b->find_run = -1;
        invalidate(wm);
    } else if (key >= 0x20 && key <= 0x7e && b->find_len < FIND_MAX - 1) {
        b->find[b->find_len++] = (char)key;
        b->find[b->find_len] = 0;
        b->find_run = -1;
        invalidate(wm);
    }
}

/* Input */

static void scroll_by(struct wm *wm, int lines)
{
    active_tab(wm)->top_line += lines;
    clamp_scroll(wm);
    invalidate(wm);
}

static void focus_link(struct wm *wm, int delta)
{
    struct browser_tab *tab = active_tab(wm);
    struct web_doc *doc = &g_docs[wm->browser.active];
    ensure_layout(wm);
    if (doc->link_count == 0)
        return;

    int next = tab->link_index < 0 ? (delta > 0 ? 0 : doc->link_count - 1) : tab->link_index + delta;
    if (next < 0)
        next = doc->link_count - 1;
    if (next >= doc->link_count)
        next = 0;
    tab->link_index = next;

    for (int i = 0; i < doc->run_count; i++) {
        if (doc->runs[i].link != next)
            continue;
        int rows = wm->windows[APP_BROWSER].open ? current_layout(wm).rows : 1;
        int line = doc->runs[i].line;
        if (line < tab->top_line)
            tab->top_line = line;
        else if (line >= tab->top_line + rows)
            tab->top_line = line - rows / 2;
        clamp_scroll(wm);
        break;
    }
    invalidate(wm);
}

static void open_link(struct wm *wm, int link, int in_new_tab)
{
    char target[URL_MAX];
    text_copy(target, sizeof target, web_link_url(&g_docs[wm->browser.active], link));
    if (!target[0])
        return;
    if (in_new_tab)
        new_tab(wm, target);
    else
        browser_navigate(wm, target);
}

void browser_key(struct wm *wm, int key)
{
    struct browser_app *b = &wm->browser;
    struct browser_tab *tab = active_tab(wm);
    int page = wm->windows[APP_BROWSER].open ? current_layout(wm).rows : 1;

    if (b->editing) {
        edit_key(wm, key);
        return;
    }
    if (b->finding && key != KEY_F5 && key != KEY_UP && key != KEY_DOWN && key != KEY_PGUP &&
        key != KEY_PGDN) {
        find_key(wm, key);
        return;
    }

    switch (key) {
    case KEY_CTRL_L:
        begin_edit(wm, 1);
        break;
    case KEY_CTRL_T:
        if (new_tab(wm, "about:home") >= 0)
            begin_edit(wm, 1);
        break;
    case KEY_CTRL_W:
        close_tab(wm, b->active);
        break;
    case KEY_CTRL_D:
        toggle_bookmark(wm);
        break;
    case KEY_CTRL_F:
        b->finding = 1;
        b->find_run = -1;
        browser_resized(wm);
        invalidate(wm);
        break;
    case KEY_F7:
        cycle_tab(wm, -1);
        break;
    case KEY_F8:
        cycle_tab(wm, 1);
        break;
    case KEY_UP:
        scroll_by(wm, -1);
        break;
    case KEY_DOWN:
        scroll_by(wm, 1);
        break;
    case KEY_PGUP:
        scroll_by(wm, -(page - 1));
        break;
    case KEY_PGDN:
        scroll_by(wm, page - 1);
        break;
    case KEY_HOME:
        scroll_by(wm, -g_docs[b->active].line_count);
        break;
    case KEY_END:
        scroll_by(wm, g_docs[b->active].line_count);
        break;
    case KEY_LEFT:
        focus_link(wm, -1);
        break;
    case KEY_RIGHT:
        focus_link(wm, 1);
        break;
    case KEY_ENTER:
        if (tab->link_index >= 0)
            open_link(wm, tab->link_index, 0);
        break;
    case KEY_BACKSPACE:
        go_history(wm, -1);
        break;
    case KEY_F5:
        reload(wm);
        break;
    case KEY_ESC:
        if (tab->state != TAB_IDLE) {
            cancel_tab(wm, b->active);
            set_status(wm, b->active, "Stopped");
        }
        break;
    default:
        if (key >= 0x20 && key <= 0x7e) {
            begin_edit(wm, 1);
            edit_key(wm, key);
        }
        break;
    }
}

static int first_run_on_line(const struct web_doc *doc, int line)
{
    int low = 0;
    int high = doc->run_count;
    while (low < high) {
        int middle = low + (high - low) / 2;
        if (doc->runs[middle].line < line)
            low = middle + 1;
        else
            high = middle;
    }
    return low;
}

/* Returns the link under a screen point, or -1. */
static int link_at(struct wm *wm, const struct browser_layout *l, int x, int y)
{
    const struct web_doc *doc = &g_docs[wm->browser.active];
    if (!rect_contains(l->page, x, y))
        return -1;

    int line = active_tab(wm)->top_line + (y - l->page.y - PAGE_PADDING) / FONT_LINE;
    int column = (x - l->text_x) / FONT_ADVANCE;
    if (x < l->text_x)
        return -1;
    for (int i = first_run_on_line(doc, line - 1); i < doc->run_count && doc->runs[i].line <= line;
         i++) {
        const struct web_run *run = &doc->runs[i];
        int height = (run->style & WEB_BIG) ? 2 : 1;
        if (run->link >= 0 && line >= run->line && line < run->line + height &&
            column >= run->column && column < run->column + web_run_width(run))
            return run->link;
    }
    return -1;
}

static void show_security_info(struct wm *wm)
{
    const struct browser_tab *tab = active_tab(wm);
    if (tab->security == SECURITY_VERIFIED)
        shell_message(wm, "Secure connection", "Encrypted, certificate verified.");
    else if (tab->security == SECURITY_PLAIN)
        shell_message(wm, "Not secure", "Plain HTTP is not encrypted.");
    else
        shell_message(wm, "Built-in page", "This page comes from the browser.");
}

void browser_press(struct wm *wm, int x, int y, int button)
{
    struct browser_app *b = &wm->browser;
    struct browser_layout l = current_layout(wm);
    struct browser_tab *tab = active_tab(wm);

    if (rect_contains(l.tabs, x, y)) {
        if (button != BUTTON_LEFT)
            return;
        int slots[BROWSER_TABS];
        int count = 0;
        for (int i = 0; i < BROWSER_TABS; i++) {
            if (b->tabs[i].used)
                slots[count++] = i;
        }
        int width = (l.tabs.w - 48) / count;
        if (width > TAB_MAX_W)
            width = TAB_MAX_W;
        for (int i = 0; i < count; i++) {
            struct rect r = { l.tabs.x + 6 + i * (width + 2), l.tabs.y + 4, width, l.tabs.h - 4 };
            if (!rect_contains(r, x, y))
                continue;
            if (x >= r.x + r.w - 22)
                close_tab(wm, slots[i]);
            else
                activate_tab(wm, slots[i]);
            return;
        }
        struct rect plus = { l.tabs.x + 6 + count * (width + 2), l.tabs.y + 4, 28, l.tabs.h - 4 };
        if (rect_contains(plus, x, y) && new_tab(wm, "about:home") >= 0)
            begin_edit(wm, 1);
        return;
    }

    if (rect_contains(l.nav, x, y)) {
        if (button != BUTTON_LEFT)
            return;
        if (rect_contains(l.address, x, y)) {
            if (x < l.address.x + 26)
                show_security_info(wm);
            else if (!b->editing)
                begin_edit(wm, 1);
            return;
        }
        b->editing = 0;
        if (rect_contains(l.back, x, y)) {
            go_history(wm, -1);
        } else if (rect_contains(l.forward, x, y)) {
            go_history(wm, 1);
        } else if (rect_contains(l.reload, x, y)) {
            if (tab->state != TAB_IDLE) {
                cancel_tab(wm, b->active);
                set_status(wm, b->active, "Stopped");
            } else {
                reload(wm);
            }
        } else if (rect_contains(l.home, x, y)) {
            browser_navigate(wm, "about:home");
        } else if (rect_contains(l.star, x, y)) {
            toggle_bookmark(wm);
        }
        invalidate(wm);
        return;
    }

    if (b->editing) {
        b->editing = 0;
        invalidate(wm);
    }

    if (rect_contains(l.bookmarks, x, y)) {
        int chip_x = l.bookmarks.x + 8;
        for (int i = 0; i < b->bookmark_count; i++) {
            int length = text_len(b->bookmark_title[i]);
            int width = (length < 16 ? length : 16) * FONT_ADVANCE + 16;
            struct rect chip = { chip_x, l.bookmarks.y + 3, width, l.bookmarks.h - 6 };
            if (rect_contains(chip, x, y)) {
                if (button == BUTTON_RIGHT)
                    new_tab(wm, b->bookmark_url[i]);
                else
                    browser_navigate(wm, b->bookmark_url[i]);
                return;
            }
            chip_x += width + 6;
        }
        return;
    }

    if (rect_contains(l.scrollbar, x, y)) {
        int max = g_docs[b->active].line_count - l.rows;
        int thumb = max > 0 ? l.scrollbar.y + l.scrollbar.h * tab->top_line / (max + l.rows) : 0;
        scroll_by(wm, y < thumb ? -(l.rows - 1) : l.rows - 1);
        return;
    }

    ensure_layout(wm);
    int link = link_at(wm, &l, x, y);
    if (link >= 0)
        open_link(wm, link, button == BUTTON_RIGHT);
}

void browser_hover(struct wm *wm, int x, int y)
{
    struct browser_layout l = current_layout(wm);
    int link = link_at(wm, &l, x, y);
    if (link != wm->browser.hover_link) {
        wm->browser.hover_link = link;
        invalidate(wm);
    }
}

void browser_wheel(struct wm *wm, int notches)
{
    char event[32] = "web:wheel:";
    scroll_by(wm, notches * WHEEL_LINES);
    text_append_u64(event, sizeof event, (uint64_t)active_tab(wm)->top_line);
    wm_trace(wm, event);
}

/* Drawing */

static uint32_t run_color(int style, int hovered)
{
    if (style & WEB_LINK)
        return hovered ? COLOR_LINK_HOVER : COLOR_LINK;
    if (style & WEB_HEADING)
        return UI_ACCENT;
    if (style & WEB_DIM)
        return UI_DIM;
    if (style & WEB_CODE)
        return COLOR_CODE;
    if (style & WEB_BOLD)
        return COLOR_BOLD;
    return UI_TEXT;
}

static void draw_button(struct canvas *c, struct rect r, const char *const *icon, int rows,
                        int enabled, uint32_t color)
{
    canvas_fill(c, r, enabled ? UI_TITLE : COLOR_CHROME);
    canvas_bitmap(c, r.x + (r.w - 14) / 2, r.y + (r.h - rows * 2) / 2, icon, rows, 2,
                  enabled ? color : UI_BORDER);
}

static void draw_tabs(struct wm *wm, const struct browser_layout *l)
{
    struct canvas *c = wm->screen;
    const struct browser_app *b = &wm->browser;
    canvas_fill(c, l->tabs, COLOR_CHROME);

    int count = tab_count(wm);
    int width = (l->tabs.w - 48) / (count ? count : 1);
    if (width > TAB_MAX_W)
        width = TAB_MAX_W;

    int index = 0;
    for (int slot = 0; slot < BROWSER_TABS; slot++) {
        const struct browser_tab *tab = &b->tabs[slot];
        if (!tab->used)
            continue;
        struct rect r = { l->tabs.x + 6 + index * (width + 2), l->tabs.y + 4, width, l->tabs.h - 4 };
        int active = slot == b->active;
        canvas_fill(c, r, active ? UI_PANEL : UI_TITLE);
        if (active)
            canvas_fill(c, (struct rect){ r.x, r.y, r.w, 2 }, UI_ACCENT);

        int text_x = r.x + 8;
        if (tab->state != TAB_IDLE) {
            canvas_fill(c, (struct rect){ r.x + 8, r.y + 10, 6, 6 }, UI_ACCENT);
            text_x += 12;
        }
        const char *label = tab->title[0] ? tab->title : tab->address[0] ? tab->address : "New Tab";
        ui_text_fit(c, text_x, r.y + 6, label, (r.x + r.w - 26 - text_x) / FONT_ADVANCE,
                    active ? UI_TEXT : UI_DIM);
        canvas_text(c, r.x + r.w - 18, r.y + 6, "x", UI_DIM);
        index++;
    }

    if (count < BROWSER_TABS) {
        struct rect plus = { l->tabs.x + 6 + count * (width + 2), l->tabs.y + 4, 28, l->tabs.h - 4 };
        canvas_fill(c, plus, UI_TITLE);
        canvas_text(c, plus.x + 8, plus.y + 6, "+", UI_TEXT);
    }
}

static void draw_nav(struct wm *wm, const struct browser_layout *l, int focused)
{
    struct canvas *c = wm->screen;
    const struct browser_app *b = &wm->browser;
    const struct browser_tab *tab = &b->tabs[b->active];

    canvas_fill(c, l->nav, UI_PANEL);
    draw_button(c, l->back, ICON_BACK, 7, tab->history_pos > 0, UI_TEXT);
    draw_button(c, l->forward, ICON_FORWARD, 7, tab->history_pos + 1 < tab->history_count, UI_TEXT);
    draw_button(c, l->reload, tab->state != TAB_IDLE ? ICON_STOP : ICON_RELOAD, 7, 1, UI_TEXT);
    draw_button(c, l->home, ICON_HOME, 7, 1, UI_TEXT);
    draw_button(c, l->star, ICON_STAR, 7, 1,
                bookmark_index(wm, tab->address) >= 0 ? UI_ACCENT : UI_DIM);

    struct rect a = l->address;
    canvas_fill(c, a, COLOR_FIELD);
    canvas_frame(c, a, b->editing && focused ? UI_ACCENT : UI_BORDER);

    /* The security indicator. */
    if (!b->editing) {
        if (tab->security == SECURITY_VERIFIED)
            canvas_bitmap(c, a.x + 8, a.y + 7, ICON_LOCK, 7, 2, COLOR_SECURE);
        else if (tab->security == SECURITY_PLAIN)
            canvas_bitmap(c, a.x + 8, a.y + 7, ICON_WARNING, 7, 2, COLOR_INSECURE);
        else
            canvas_bitmap(c, a.x + 8, a.y + 7, ICON_INFO, 7, 2, UI_DIM);
    }

    int text_x = a.x + 26;
    int columns = (a.x + a.w - 8 - text_x) / FONT_ADVANCE;
    if (b->editing) {
        const char *text = b->edit;
        if (b->edit_len >= columns)
            text = b->edit + b->edit_len - columns + 1;
        if (b->edit_fresh && b->edit_len > 0) {
            int shown = text_len(text);
            canvas_fill(c, (struct rect){ text_x - 1, a.y + 5, shown * FONT_ADVANCE + 2, 18 },
                        UI_TITLE_FOCUS);
        }
        canvas_text(c, text_x, a.y + 7, text, UI_TEXT);
        if (focused)
            canvas_fill(c, (struct rect){ text_x + text_len(text) * FONT_ADVANCE, a.y + 5, 2, 18 },
                        UI_ACCENT);
    } else {
        /* Show the scheme dimmed so the site name stands out. */
        const char *address = tab->address;
        int scheme = text_starts(address, "https://") ? 8 : text_starts(address, "http://") ? 7 : 0;
        char host[URL_MAX];
        text_copy(host, sizeof host, address + scheme);
        if (scheme > 0 && columns > scheme) {
            char prefix[9];
            text_copy(prefix, scheme + 1, address);
            canvas_text(c, text_x, a.y + 7, prefix, UI_DIM);
            ui_text_fit(c, text_x + scheme * FONT_ADVANCE, a.y + 7, host, columns - scheme, UI_TEXT);
        } else {
            ui_text_fit(c, text_x, a.y + 7, address, columns, UI_TEXT);
        }
    }
}

static void draw_bookmarks(struct wm *wm, const struct browser_layout *l)
{
    struct canvas *c = wm->screen;
    const struct browser_app *b = &wm->browser;
    canvas_fill(c, l->bookmarks, UI_PANEL);
    canvas_fill(c, (struct rect){ l->bookmarks.x, l->bookmarks.y + l->bookmarks.h - 1,
                                  l->bookmarks.w, 1 }, UI_BORDER);

    int x = l->bookmarks.x + 8;
    for (int i = 0; i < b->bookmark_count; i++) {
        int length = text_len(b->bookmark_title[i]);
        int width = (length < 16 ? length : 16) * FONT_ADVANCE + 16;
        if (x + width > l->bookmarks.x + l->bookmarks.w - 8)
            break;
        struct rect chip = { x, l->bookmarks.y + 3, width, l->bookmarks.h - 6 };
        canvas_fill(c, chip, UI_TITLE);
        ui_text_fit(c, chip.x + 8, chip.y + 3, b->bookmark_title[i], 16, UI_TEXT);
        x += width + 6;
    }
    if (b->bookmark_count == 0)
        canvas_text(c, x, l->bookmarks.y + 6, "Press Ctrl+D to bookmark a page", UI_DIM);
}

static void draw_page(struct wm *wm, const struct browser_layout *l)
{
    struct canvas *c = wm->screen;
    const struct browser_app *b = &wm->browser;
    const struct browser_tab *tab = &b->tabs[b->active];
    const struct web_doc *doc = &g_docs[b->active];

    canvas_fill(c, l->page, COLOR_PAGE);
    struct rect saved = c->clip;
    struct rect visible;
    if (!rect_intersect(l->page, saved, &visible))
        return;
    c->clip = visible;

    int previous_line = -1;
    int top = tab->top_line;
    for (int i = first_run_on_line(doc, top - 1); i < doc->run_count; i++) {
        const struct web_run *run = &doc->runs[i];
        if (run->line >= top + l->rows)
            break;
        int scale = (run->style & WEB_BIG) ? 2 : 1;
        if (run->line + scale <= top)
            continue;

        int x = l->text_x + run->column * FONT_ADVANCE;
        int y = l->page.y + PAGE_PADDING + (run->line - top) * FONT_LINE;
        int first_on_line = run->line != previous_line;
        previous_line = run->line;

        if (run->style & WEB_RULE) {
            canvas_fill(c, (struct rect){ l->text_x, y + 8, l->columns * FONT_ADVANCE, 1 }, UI_BORDER);
            continue;
        }
        if ((run->style & WEB_QUOTE) && first_on_line && run->column >= 2)
            canvas_fill(c, (struct rect){ x - 14, y - 2, 3, FONT_LINE }, UI_ACCENT);

        int width = web_run_width(run) * FONT_ADVANCE;
        int height = FONT_LINE * scale;
        if (i == b->find_run)
            canvas_fill(c, (struct rect){ x - 2, y - 2, width + 4, height }, COLOR_MATCH);
        else if (run->link >= 0 && run->link == tab->link_index)
            canvas_fill(c, (struct rect){ x - 2, y - 2, width + 4, height }, UI_TITLE_FOCUS);
        else if (run->style & WEB_CODE)
            canvas_fill(c, (struct rect){ x - 2, y - 2, width + 4, height }, COLOR_CODE_BG);

        char text[RUN_DRAW_MAX + 1];
        int count = run->length < RUN_DRAW_MAX ? run->length : RUN_DRAW_MAX;
        for (int k = 0; k < count; k++)
            text[k] = doc->text[run->text + (uint32_t)k];
        text[count] = 0;

        int hovered = run->link >= 0 && run->link == b->hover_link;
        canvas_text_scaled(c, x, y, text, run_color(run->style, hovered), scale);
        if (run->style & WEB_LINK)
            canvas_fill(c, (struct rect){ x, y + (FONT_HEIGHT + 1) * scale, width - 2, hovered ? 2 : 1 },
                        run_color(run->style, hovered));
    }

    /* A thin progress bar across the top while loading. */
    if (tab->state != TAB_IDLE) {
        struct rect bar = { l->page.x, l->page.y, l->page.w, 3 };
        if (b->loading_tab == b->active && g_http.phase == HTTP_BODY && g_http.has_length &&
            g_http.content_length > 0) {
            int done = (int)((int64_t)bar.w * g_http.body_length / (int64_t)g_http.content_length);
            canvas_fill(c, (struct rect){ bar.x, bar.y, done, bar.h }, UI_ACCENT);
        } else {
            int span = bar.w / 4;
            int offset = (int)((wm->ticks * 8) % (uint32_t)(bar.w + span)) - span;
            canvas_fill(c, (struct rect){ bar.x + offset, bar.y, span, bar.h }, UI_ACCENT);
        }
    }
    c->clip = saved;
}

static void draw_scrollbar(struct wm *wm, const struct browser_layout *l)
{
    struct canvas *c = wm->screen;
    const struct web_doc *doc = &g_docs[wm->browser.active];
    canvas_fill(c, l->scrollbar, COLOR_PAGE);
    if (doc->line_count <= l->rows)
        return;

    int thumb_h = l->scrollbar.h * l->rows / doc->line_count;
    if (thumb_h < 16)
        thumb_h = 16;
    int max = doc->line_count - l->rows;
    int thumb_y = l->scrollbar.y + (l->scrollbar.h - thumb_h) * active_tab(wm)->top_line / max;
    canvas_fill(c, (struct rect){ l->scrollbar.x + 2, thumb_y, l->scrollbar.w - 4, thumb_h }, UI_DIM);
}

static void draw_find(struct wm *wm, const struct browser_layout *l)
{
    struct canvas *c = wm->screen;
    const struct browser_app *b = &wm->browser;
    struct rect f = l->find;
    canvas_fill(c, f, UI_PANEL);
    canvas_fill(c, (struct rect){ f.x, f.y, f.w, 1 }, UI_BORDER);
    canvas_text(c, f.x + 10, f.y + 9, "Find:", UI_DIM);
    struct rect field = { f.x + 80, f.y + 4, 300, 22 };
    canvas_fill(c, field, COLOR_FIELD);
    canvas_frame(c, field, UI_ACCENT);
    ui_text_fit(c, field.x + 6, field.y + 4, b->find, (field.w - 16) / FONT_ADVANCE, UI_TEXT);
    canvas_text(c, field.x + field.w + 12, f.y + 9, "Enter: next   Esc: close", UI_DIM);
}

static void draw_status(struct wm *wm, const struct browser_layout *l)
{
    struct canvas *c = wm->screen;
    const struct browser_app *b = &wm->browser;
    const struct browser_tab *tab = &b->tabs[b->active];
    struct rect s = l->status;

    canvas_fill(c, s, UI_PANEL);
    canvas_fill(c, (struct rect){ s.x, s.y, s.w, 1 }, UI_BORDER);

    const char *security = tab->security == SECURITY_VERIFIED ? "Secure (TLS 1.2)"
                           : tab->security == SECURITY_PLAIN  ? "Not secure"
                                                              : "";
    int security_width = text_len(security) * FONT_ADVANCE;
    if (security[0])
        canvas_text(c, s.x + s.w - security_width - 10, s.y + 5, security,
                    tab->security == SECURITY_VERIFIED ? COLOR_SECURE : COLOR_INSECURE);

    const char *text = tab->status;
    uint32_t color = UI_DIM;
    if (b->hover_link >= 0) {
        text = web_link_url(&g_docs[b->active], b->hover_link);
        color = COLOR_LINK;
    } else if (tab->link_index >= 0) {
        text = web_link_url(&g_docs[b->active], tab->link_index);
        color = COLOR_LINK;
    }
    ui_text_fit(c, s.x + 8, s.y + 5, text, (s.w - security_width - 36) / FONT_ADVANCE, color);
}

void browser_draw(struct wm *wm, struct rect content, int focused)
{
    struct browser_layout l = compute_layout(wm, content);
    ensure_layout(wm);
    clamp_scroll(wm);

    draw_tabs(wm, &l);
    draw_nav(wm, &l, focused);
    draw_bookmarks(wm, &l);
    draw_page(wm, &l);
    draw_scrollbar(wm, &l);
    if (wm->browser.finding)
        draw_find(wm, &l);
    draw_status(wm, &l);
}
