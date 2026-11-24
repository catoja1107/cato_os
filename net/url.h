/* URL parsing and resolution for the web browser (a practical subset of RFC 3986). */
#ifndef CATO_NET_URL_H
#define CATO_NET_URL_H

#define URL_MAX 512
#define URL_HOST_MAX 128
#define URL_PATH_MAX 360 /* Scheme, host, port, and path always fit in URL_MAX together. */

enum url_scheme { URL_HTTP, URL_HTTPS, URL_ABOUT, URL_OTHER };

struct url {
    int scheme;
    int port;
    char host[URL_HOST_MAX];
    char path[URL_PATH_MAX]; /* Includes the query, never the fragment. */
};

/* Text without a scheme is treated as http. Returns 1 on success. */
int url_parse(const char *text, struct url *out);
/* Resolves a link against the page it appears on. Returns 1 on success. */
int url_resolve(const struct url *base, const char *reference, struct url *out);
void url_format(const struct url *url, char *out, int capacity);

#endif
