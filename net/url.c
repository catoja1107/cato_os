#include "url.h"

#include "text.h"

static const char HEX[] = "0123456789ABCDEF";

static char lower(char c)
{
    return c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c;
}

static int is_alpha(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static int is_digit(char c)
{
    return c >= '0' && c <= '9';
}

/* Returns the length of a leading "scheme:", or -1 if there is none. */
static int scheme_length(const char *text, int length)
{
    if (length == 0 || !is_alpha(text[0]))
        return -1;
    for (int i = 1; i < length; i++) {
        char c = text[i];
        if (c == ':')
            return i;
        if (!is_alpha(c) && !is_digit(c) && c != '+' && c != '-' && c != '.')
            return -1;
    }
    return -1;
}

static int scheme_equals(const char *text, int length, const char *name)
{
    if (text_len(name) != length)
        return 0;
    for (int i = 0; i < length; i++) {
        if (lower(text[i]) != name[i])
            return 0;
    }
    return 1;
}

/* Removes "." and ".." segments from the path portion, leaving the query alone. */
static void normalize_path(char *path)
{
    char out[URL_PATH_MAX];
    int o = 0;
    int i = 0;
    int ends_in_dir = 0;

    while (path[i] && path[i] != '?') {
        int start = i + 1;
        int end = start;
        while (path[end] && path[end] != '/' && path[end] != '?')
            end++;
        int length = end - start;
        ends_in_dir = 0;

        if (length == 1 && path[start] == '.') {
            ends_in_dir = 1;
        } else if (length == 2 && path[start] == '.' && path[start + 1] == '.') {
            while (o > 0 && out[o - 1] != '/')
                o--;
            if (o > 0)
                o--;
            ends_in_dir = 1;
        } else {
            out[o++] = '/';
            for (int k = start; k < end; k++)
                out[o++] = path[k];
        }
        i = end;
    }

    if (ends_in_dir || o == 0)
        out[o++] = '/';
    while (path[i] && o < URL_PATH_MAX - 1)
        out[o++] = path[i++];
    out[o] = 0;
    text_copy(path, URL_PATH_MAX, out);
}

static int parse_authority(const char *text, int length, struct url *out)
{
    int host_length = 0;
    int i = 0;

    for (; i < length && text[i] != ':'; i++) {
        char c = lower(text[i]);
        /* Credentials and IPv6 literals are rejected, which also blocks user@host spoofing. */
        if (!(is_alpha(c) || is_digit(c) || c == '.' || c == '-' || c == '_'))
            return 0;
        if (host_length >= URL_HOST_MAX - 1)
            return 0;
        out->host[host_length++] = c;
    }
    out->host[host_length] = 0;
    if (host_length == 0)
        return 0;

    out->port = out->scheme == URL_HTTPS ? 443 : 80;
    if (i < length) {
        int port = 0;
        i++;
        if (i >= length)
            return 0;
        for (; i < length; i++) {
            if (!is_digit(text[i]))
                return 0;
            port = port * 10 + (text[i] - '0');
            if (port > 65535)
                return 0;
        }
        if (port == 0)
            return 0;
        out->port = port;
    }
    return 1;
}

static int parse_path(const char *text, int length, struct url *out)
{
    int o = 0;
    if (length == 0 || text[0] != '/')
        out->path[o++] = '/';

    for (int i = 0; i < length; i++) {
        unsigned char c = (unsigned char)text[i];
        if (c == '#')
            break;
        if (c <= 0x20 || c >= 0x7f || c == '"' || c == '<' || c == '>' || c == '\\' ||
            c == '^' || c == '`' || c == '{' || c == '|' || c == '}') {
            if (o + 3 >= URL_PATH_MAX)
                return 0;
            out->path[o++] = '%';
            out->path[o++] = HEX[c >> 4];
            out->path[o++] = HEX[c & 15];
        } else {
            if (o + 1 >= URL_PATH_MAX)
                return 0;
            out->path[o++] = (char)c;
        }
    }
    out->path[o] = 0;
    normalize_path(out->path);
    return 1;
}

int url_parse(const char *text, struct url *out)
{
    while (*text == ' ' || *text == '\t' || *text == '\r' || *text == '\n')
        text++;
    int length = text_len(text);
    while (length > 0 && (text[length - 1] == ' ' || text[length - 1] == '\t' ||
                          text[length - 1] == '\r' || text[length - 1] == '\n'))
        length--;
    if (length == 0 || length >= URL_MAX)
        return 0;

    /* Control characters could otherwise inject HTTP headers. */
    for (int i = 0; i < length; i++) {
        unsigned char c = (unsigned char)text[i];
        if (c < 0x20 || c == 0x7f)
            return 0;
    }

    out->scheme = URL_HTTP;
    out->port = 0;
    out->host[0] = 0;
    out->path[0] = 0;

    int position = 0;
    int colon = scheme_length(text, length);
    if (colon > 0) {
        int has_slashes = colon + 2 < length && text[colon + 1] == '/' && text[colon + 2] == '/';
        if (has_slashes) {
            if (scheme_equals(text, colon, "http"))
                out->scheme = URL_HTTP;
            else if (scheme_equals(text, colon, "https"))
                out->scheme = URL_HTTPS;
            else {
                out->scheme = URL_OTHER;
                return 1;
            }
            position = colon + 3;
        } else if (scheme_equals(text, colon, "about")) {
            int o = 0;
            out->scheme = URL_ABOUT;
            for (int i = colon + 1; i < length && o < URL_PATH_MAX - 1; i++)
                out->path[o++] = lower(text[i]);
            out->path[o] = 0;
            return 1;
        } else if (colon + 1 >= length || !is_digit(text[colon + 1])) {
            /* mailto:, javascript:, data:, and similar are never fetched. */
            out->scheme = URL_OTHER;
            return 1;
        }
        /* Otherwise this is host:port with no scheme. */
    }

    int end = position;
    while (end < length && text[end] != '/' && text[end] != '?' && text[end] != '#')
        end++;
    if (!parse_authority(text + position, end - position, out))
        return 0;
    return parse_path(text + end, length - end, out);
}

void url_format(const struct url *url, char *out, int capacity)
{
    if (url->scheme == URL_ABOUT) {
        text_copy(out, capacity, "about:");
        text_append(out, capacity, url->path);
        return;
    }
    text_copy(out, capacity, url->scheme == URL_HTTPS ? "https://" : "http://");
    text_append(out, capacity, url->host);
    if (url->port != (url->scheme == URL_HTTPS ? 443 : 80)) {
        text_append(out, capacity, ":");
        text_append_u64(out, capacity, (uint64_t)url->port);
    }
    text_append(out, capacity, url->path);
}

int url_resolve(const struct url *base, const char *reference, struct url *out)
{
    while (*reference == ' ' || *reference == '\t' || *reference == '\r' || *reference == '\n')
        reference++;
    if (*reference == 0 || *reference == '#') {
        *out = *base;
        return 1;
    }

    int length = text_len(reference);
    if (scheme_length(reference, length) > 0)
        return url_parse(reference, out);
    if (base->scheme != URL_HTTP && base->scheme != URL_HTTPS)
        return 0;

    char combined[URL_MAX * 2];
    if (reference[0] == '/' && reference[1] == '/') {
        text_copy(combined, sizeof combined, base->scheme == URL_HTTPS ? "https:" : "http:");
        text_append(combined, sizeof combined, reference);
        return text_len(combined) < URL_MAX && url_parse(combined, out);
    }

    struct url origin = *base;
    origin.path[0] = 0;
    url_format(&origin, combined, sizeof combined);

    if (reference[0] != '/') {
        int query = 0;
        while (base->path[query] && base->path[query] != '?')
            query++;

        if (reference[0] == '?') {
            int position = text_len(combined);
            for (int i = 0; i < query && position < (int)sizeof combined - 1; i++)
                combined[position++] = base->path[i];
            combined[position] = 0;
        } else {
            int slash = query;
            while (slash > 0 && base->path[slash - 1] != '/')
                slash--;
            int position = text_len(combined);
            for (int i = 0; i < slash && position < (int)sizeof combined - 1; i++)
                combined[position++] = base->path[i];
            combined[position] = 0;
        }
    }
    text_append(combined, sizeof combined, reference);
    return text_len(combined) < URL_MAX && url_parse(combined, out);
}
