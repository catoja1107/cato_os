#include "http.h"

#include "text.h"
#include "tls.h"

#define IDLE_TIMEOUT_MS 20000u
#define READ_CHUNK 2048
#define REQUEST_MAX 2048

/* One TLS session at a time, matching the single TCP connection. */
static struct tls_session g_tls;

/* A generic client name, so requests do not identify this project or its owner. */
#define USER_AGENT "SimpleBrowser/0.1"

enum chunk_state { CHUNK_SIZE, CHUNK_EXTENSION, CHUNK_SIZE_LF, CHUNK_DATA, CHUNK_DATA_CR, CHUNK_DATA_LF };

static char lower(char c)
{
    return c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c;
}

static int name_equals(const char *text, int length, const char *name)
{
    if (text_len(name) != length)
        return 0;
    for (int i = 0; i < length; i++) {
        if (lower(text[i]) != name[i])
            return 0;
    }
    return 1;
}

static int contains_word(const char *text, int length, const char *word)
{
    int size = text_len(word);
    for (int i = 0; i + size <= length; i++) {
        int k = 0;
        while (k < size && lower(text[i + k]) == word[k])
            k++;
        if (k == size)
            return 1;
    }
    return 0;
}

static void fail(struct http_client *http, const char *message)
{
    tcp_abort(http->net);
    text_copy(http->error, sizeof http->error, message);
    http->phase = HTTP_FAILED;
}

static void finish(struct http_client *http, int clean)
{
    if (clean) {
        if (http->secure)
            tls_close(&g_tls, http->net);
        tcp_close(http->net);
    } else {
        tcp_abort(http->net);
    }
    http->phase = HTTP_DONE;
}

static int transport_read(struct http_client *http, uint8_t *out, int capacity)
{
    return http->secure ? tls_read(&g_tls, http->net, out, capacity)
                        : tcp_read(http->net, out, capacity);
}

static int transport_finished(struct http_client *http)
{
    return http->secure ? tls_finished(&g_tls, http->net) : tcp_is_finished(http->net);
}

/* True if the connection broke, as opposed to ending normally. */
static int transport_broken(struct http_client *http)
{
    if (http->net->tcp.error)
        return 1;
    return http->secure && tls_closed(&g_tls) && tls_error(&g_tls) != 0;
}

void http_start(struct http_client *http, struct net *net, const struct url *url, uint8_t *body,
                int body_capacity)
{
    http->net = net;
    http->url = *url;
    http->body = body;
    http->body_capacity = body_capacity;
    http->body_length = 0;
    http->truncated = 0;
    http->head_length = 0;
    http->status = 0;
    http->has_length = 0;
    http->content_length = 0;
    http->chunked = 0;
    http->chunk_state = CHUNK_SIZE;
    http->chunk_left = 0;
    http->chunk_digits = 0;
    http->content_type[0] = 0;
    http->location[0] = 0;
    http->error[0] = 0;
    http->secure = url->scheme == URL_HTTPS;
    http->tls_version = 0;
    http->tls_cipher = 0;
    http->deadline = net->now + IDLE_TIMEOUT_MS;

    uint32_t literal;
    if (net_parse_ip(url->host, &literal)) {
        http->phase = HTTP_CONNECTING;
        if (!tcp_connect(net, literal, (uint16_t)url->port))
            fail(http, "The connection could not be started.");
    } else {
        http->phase = HTTP_RESOLVING;
        if (!dns_resolve(net, url->host))
            fail(http, "That site name could not be looked up.");
    }
}

void http_cancel(struct http_client *http)
{
    if (http->phase != HTTP_IDLE && http->phase != HTTP_DONE && http->phase != HTTP_FAILED)
        tcp_abort(http->net);
    http->phase = HTTP_IDLE;
}

static void send_request(struct http_client *http)
{
    char request[REQUEST_MAX];
    const struct url *url = &http->url;

    text_copy(request, sizeof request, "GET ");
    text_append(request, sizeof request, url->path);
    text_append(request, sizeof request, " HTTP/1.1\r\nHost: ");
    text_append(request, sizeof request, url->host);
    if (url->port != (http->secure ? 443 : 80)) {
        text_append(request, sizeof request, ":");
        text_append_u64(request, sizeof request, (uint64_t)url->port);
    }
    text_append(request, sizeof request,
                "\r\nUser-Agent: " USER_AGENT
                "\r\nAccept: text/html,text/plain;q=0.9,*/*;q=0.1"
                "\r\nAccept-Encoding: identity\r\nConnection: close\r\n\r\n");

    int length = text_len(request);
    int sent = 0;
    if (http->secure) {
        while (sent < length) {
            int count = tls_write(&g_tls, http->net, (const uint8_t *)request + sent, length - sent);
            if (count <= 0)
                break;
            sent += count;
        }
    } else if (tcp_send(http->net, (const uint8_t *)request, length)) {
        sent = length;
    }

    if (sent != length)
        fail(http, "The request could not be sent.");
    else
        http->phase = HTTP_HEADERS;
}

static void start_tls(struct http_client *http)
{
    struct net *net = http->net;
    uint8_t seed[TLS_SEED_BYTES];

    if (!net->driver.entropy || !net->driver.entropy(net->driver.context, seed, sizeof seed)) {
        fail(http, "This computer has no hardware random number source, so HTTPS is unavailable.");
        return;
    }
    if (net->clock_days == 0) {
        fail(http, "The system clock is not set, so the site's certificate cannot be checked.");
        return;
    }
    int started = tls_begin(&g_tls, http->url.host, net->clock_days, net->clock_seconds, seed,
                            sizeof seed);

    /* Do not leave key material lying around on the stack. */
    volatile uint8_t *wipe = seed;
    for (unsigned i = 0; i < sizeof seed; i++)
        wipe[i] = 0;

    if (!started) {
        fail(http, "The secure connection could not be started.");
        return;
    }
    http->phase = HTTP_HANDSHAKE;
    http->deadline = net->now + IDLE_TIMEOUT_MS;
    tls_pump(&g_tls, net);
}

/* Parses the buffered response head. Returns 0 after failing the request. */
static int parse_head(struct http_client *http, int length)
{
    const char *head = http->head;
    int line_end = 0;
    while (line_end + 1 < length && !(head[line_end] == '\r' && head[line_end + 1] == '\n'))
        line_end++;

    /* Status line: HTTP/1.x NNN reason */
    if (line_end < 12 || head[0] != 'H' || head[1] != 'T' || head[2] != 'T' || head[3] != 'P' ||
        head[4] != '/' || head[8] != ' ' || head[9] < '1' || head[9] > '5' ||
        head[10] < '0' || head[10] > '9' || head[11] < '0' || head[11] > '9') {
        fail(http, "The server sent an invalid response.");
        return 0;
    }
    http->status = (head[9] - '0') * 100 + (head[10] - '0') * 10 + (head[11] - '0');

    int position = line_end + 2;
    while (position + 1 < length) {
        int end = position;
        while (end + 1 < length && !(head[end] == '\r' && head[end + 1] == '\n'))
            end++;
        if (end == position)
            break;

        int colon = position;
        while (colon < end && head[colon] != ':')
            colon++;
        if (colon < end) {
            const char *name = head + position;
            int name_length = colon - position;
            int value_start = colon + 1;
            while (value_start < end && (head[value_start] == ' ' || head[value_start] == '\t'))
                value_start++;
            int value_end = end;
            while (value_end > value_start &&
                   (head[value_end - 1] == ' ' || head[value_end - 1] == '\t'))
                value_end--;
            const char *value = head + value_start;
            int value_length = value_end - value_start;

            if (name_equals(name, name_length, "content-length")) {
                uint64_t number = 0;
                int valid = value_length > 0 && value_length <= 10;
                for (int i = 0; valid && i < value_length; i++) {
                    if (value[i] < '0' || value[i] > '9')
                        valid = 0;
                    else
                        number = number * 10 + (uint64_t)(value[i] - '0');
                }
                if (!valid || number > 0xffffffffu) {
                    fail(http, "The server sent an invalid length.");
                    return 0;
                }
                http->has_length = 1;
                http->content_length = (uint32_t)number;
            } else if (name_equals(name, name_length, "transfer-encoding")) {
                if (contains_word(value, value_length, "chunked"))
                    http->chunked = 1;
                else if (!name_equals(value, value_length, "identity")) {
                    fail(http, "The server used an unsupported encoding.");
                    return 0;
                }
            } else if (name_equals(name, name_length, "content-encoding")) {
                if (value_length > 0 && !name_equals(value, value_length, "identity")) {
                    fail(http, "The server sent compressed data.");
                    return 0;
                }
            } else if (name_equals(name, name_length, "content-type")) {
                int count = value_length < (int)sizeof http->content_type - 1
                                ? value_length
                                : (int)sizeof http->content_type - 1;
                for (int i = 0; i < count; i++)
                    http->content_type[i] = lower(value[i]);
                http->content_type[count] = 0;
            } else if (name_equals(name, name_length, "location")) {
                if (value_length < URL_MAX) {
                    for (int i = 0; i < value_length; i++)
                        http->location[i] = value[i];
                    http->location[value_length] = 0;
                }
            }
        }
        position = end + 2;
    }

    if (http->chunked)
        http->has_length = 0;
    return 1;
}

/* Stores body bytes. Returns 0 once the buffer is full. */
static int store(struct http_client *http, const uint8_t *data, int length)
{
    int space = http->body_capacity - http->body_length;
    int count = length < space ? length : space;
    for (int i = 0; i < count; i++)
        http->body[http->body_length + i] = data[i];
    http->body_length += count;
    if (count < length) {
        http->truncated = 1;
        return 0;
    }
    return 1;
}

static int hex_value(uint8_t c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

/* Decodes chunked data. Returns 1 to continue, 0 when finished, and -1 on bad framing. */
static int feed_chunked(struct http_client *http, const uint8_t *data, int length)
{
    int i = 0;
    while (i < length) {
        uint8_t c = data[i];
        switch (http->chunk_state) {
        case CHUNK_SIZE: {
            int digit = hex_value(c);
            if (digit >= 0) {
                if (++http->chunk_digits > 7)
                    return -1;
                http->chunk_left = http->chunk_left * 16 + (uint32_t)digit;
            } else if (http->chunk_digits == 0) {
                return -1;
            } else if (c == ';' || c == ' ' || c == '\t') {
                http->chunk_state = CHUNK_EXTENSION;
            } else if (c == '\r') {
                http->chunk_state = CHUNK_SIZE_LF;
            } else {
                return -1;
            }
            i++;
            break;
        }
        case CHUNK_EXTENSION:
            if (c == '\r')
                http->chunk_state = CHUNK_SIZE_LF;
            i++;
            break;
        case CHUNK_SIZE_LF:
            if (c != '\n')
                return -1;
            i++;
            if (http->chunk_left == 0)
                return 0; /* Trailers are ignored. */
            http->chunk_state = CHUNK_DATA;
            break;
        case CHUNK_DATA: {
            int available = length - i;
            int count = (uint32_t)available < http->chunk_left ? available : (int)http->chunk_left;
            if (!store(http, data + i, count))
                return 0;
            i += count;
            http->chunk_left -= (uint32_t)count;
            if (http->chunk_left == 0)
                http->chunk_state = CHUNK_DATA_CR;
            break;
        }
        case CHUNK_DATA_CR:
            if (c != '\r')
                return -1;
            http->chunk_state = CHUNK_DATA_LF;
            i++;
            break;
        default:
            if (c != '\n')
                return -1;
            http->chunk_state = CHUNK_SIZE;
            http->chunk_digits = 0;
            http->chunk_left = 0;
            i++;
            break;
        }
    }
    return 1;
}

static void feed_body(struct http_client *http, const uint8_t *data, int length)
{
    if (http->chunked) {
        int result = feed_chunked(http, data, length);
        if (result < 0) {
            http->truncated = 1;
            finish(http, 0);
        } else if (result == 0) {
            finish(http, !http->truncated);
        }
        return;
    }

    if (http->has_length) {
        uint32_t remaining = http->content_length - (uint32_t)http->body_length;
        if ((uint32_t)length > remaining)
            length = (int)remaining;
    }
    if (!store(http, data, length)) {
        finish(http, 0);
        return;
    }
    if (http->has_length && (uint32_t)http->body_length >= http->content_length)
        finish(http, 1);
}

static void feed_head(struct http_client *http, const uint8_t *data, int length)
{
    for (int i = 0; i < length; i++) {
        if (http->head_length >= HTTP_HEAD_MAX) {
            fail(http, "The server sent too many headers.");
            return;
        }
        http->head[http->head_length++] = (char)data[i];

        int n = http->head_length;
        if (n >= 4 && http->head[n - 4] == '\r' && http->head[n - 3] == '\n' &&
            http->head[n - 2] == '\r' && http->head[n - 1] == '\n') {
            if (!parse_head(http, n))
                return;
            if (http->status == 204 || http->status == 304 ||
                (http->has_length && http->content_length == 0)) {
                finish(http, 1);
                return;
            }
            http->phase = HTTP_BODY;
            if (i + 1 < length)
                feed_body(http, data + i + 1, length - i - 1);
            return;
        }
    }
}

static const char *tcp_failure_text(int error)
{
    switch (error) {
    case TCP_ERR_REFUSED:
        return "The server refused the connection.";
    case TCP_ERR_RESET:
        return "The connection was reset.";
    default:
        return "The connection timed out.";
    }
}

static void poll_transfer(struct http_client *http)
{
    struct net *net = http->net;
    uint8_t buffer[READ_CHUNK];

    for (;;) {
        int count = transport_read(http, buffer, sizeof buffer);
        if (count <= 0)
            break;
        http->deadline = net->now + IDLE_TIMEOUT_MS;
        if (http->phase == HTTP_HEADERS)
            feed_head(http, buffer, count);
        else
            feed_body(http, buffer, count);
        if (http->phase != HTTP_HEADERS && http->phase != HTTP_BODY)
            return;
    }

    if (transport_finished(http)) {
        if (http->phase == HTTP_HEADERS) {
            if (http->secure && tls_closed(&g_tls) && tls_error(&g_tls) != 0)
                fail(http, tls_error_text(tls_error(&g_tls)));
            else
                fail(http, net->tcp.error ? tcp_failure_text(net->tcp.error)
                                          : "The server closed the connection.");
        } else {
            /* Without a length, the end of the connection marks the end of the body. */
            if (transport_broken(http) || http->chunked ||
                (http->has_length && (uint32_t)http->body_length < http->content_length))
                http->truncated = 1;
            finish(http, !http->truncated);
        }
    } else if (net_time_reached(net->now, http->deadline)) {
        fail(http, "The server stopped responding.");
    }
}

void http_poll(struct http_client *http)
{
    struct net *net = http->net;

    switch (http->phase) {
    case HTTP_RESOLVING:
        if (net->dns.state == DNS_DONE) {
            http->phase = HTTP_CONNECTING;
            http->deadline = net->now + IDLE_TIMEOUT_MS;
            if (!tcp_connect(net, net->dns.address, (uint16_t)http->url.port))
                fail(http, "The connection could not be started.");
        } else if (net->dns.state == DNS_NOT_FOUND) {
            fail(http, "That site name does not exist.");
        } else if (net->dns.state != DNS_WAITING) {
            fail(http, "That site name could not be looked up.");
        }
        break;
    case HTTP_CONNECTING:
        if (tcp_is_established(net)) {
            if (http->secure)
                start_tls(http);
            else
                send_request(http);
        } else if (net->tcp.state == TCP_CLOSED) {
            fail(http, tcp_failure_text(net->tcp.error));
        } else if (net_time_reached(net->now, http->deadline)) {
            fail(http, "The connection timed out.");
        }
        break;
    case HTTP_HANDSHAKE:
        tls_pump(&g_tls, net);
        if (tls_ready(&g_tls)) {
            tls_describe(&g_tls, &http->tls_version, &http->tls_cipher);
            send_request(http);
        } else if (tls_closed(&g_tls)) {
            fail(http, tls_error_text(tls_error(&g_tls)));
        } else if (net->tcp.state == TCP_CLOSED) {
            fail(http, tcp_failure_text(net->tcp.error));
        } else if (net_time_reached(net->now, http->deadline)) {
            fail(http, "The secure connection timed out.");
        }
        break;
    case HTTP_HEADERS:
    case HTTP_BODY:
        poll_transfer(http);
        break;
    default:
        break;
    }
}
