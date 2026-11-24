/* Polled HTTP/1.1 GET client (RFC 9110 and RFC 9112), over plain TCP or TLS. */
#ifndef CATO_NET_HTTP_H
#define CATO_NET_HTTP_H

#include "net.h"
#include "url.h"

#define HTTP_HEAD_MAX 8192
#define HTTP_BODY_MAX (2 * 1024 * 1024)

enum http_phase {
    HTTP_IDLE,
    HTTP_RESOLVING,
    HTTP_CONNECTING,
    HTTP_HANDSHAKE,
    HTTP_HEADERS,
    HTTP_BODY,
    HTTP_DONE,
    HTTP_FAILED,
};

struct http_client {
    struct net *net;
    int phase;
    struct url url;
    uint32_t deadline;
    int secure;
    int tls_version; /* For example 0x0303 for TLS 1.2, once connected. */
    int tls_cipher;

    uint8_t *body;
    int body_capacity;
    int body_length;
    int truncated; /* The page was larger than the buffer, or ended early. */

    char head[HTTP_HEAD_MAX];
    int head_length;
    int status;
    int has_length;
    uint32_t content_length;
    int chunked;
    int chunk_state;
    uint32_t chunk_left;
    int chunk_digits;

    char content_type[64];
    char location[URL_MAX];
    char error[128];
};

/* Starts a request. The network must be ready. */
void http_start(struct http_client *http, struct net *net, const struct url *url, uint8_t *body,
                int body_capacity);
void http_poll(struct http_client *http);
void http_cancel(struct http_client *http);

#endif
