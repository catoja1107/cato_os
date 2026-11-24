/* DNS stub resolver for IPv4 addresses (RFC 1035). */
#include "net.h"

#define DNS_PORT 53
#define HEADER_SIZE 12
#define TYPE_A 1
#define CLASS_IN 1
#define RETRY_MS 2000u
#define MAX_RETRIES 3
#define RCODE_NXDOMAIN 3

static char lower(uint8_t c)
{
    return c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : (char)c;
}

/* Encodes a host name as DNS labels. Returns the encoded length, or 0 if invalid. */
static int encode_name(const char *name, uint8_t *out, int capacity)
{
    int o = 0;
    while (*name) {
        int length = 0;
        while (name[length] && name[length] != '.') {
            char c = name[length];
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                  c == '-' || c == '_'))
                return 0;
            length++;
        }
        if (length == 0 || length > 63 || o + length + 2 > capacity || o + length + 2 > 255)
            return 0;
        out[o++] = (uint8_t)length;
        for (int i = 0; i < length; i++)
            out[o++] = (uint8_t)name[i];
        name += length;
        if (*name == '.')
            name++;
    }
    if (o == 0)
        return 0;
    out[o++] = 0;
    return o;
}

static void send_query(struct net *net)
{
    struct dns_client *dns = &net->dns;
    net_udp_send(net, net->dns_server, dns->port, DNS_PORT, dns->query, dns->query_len);
    dns->deadline = net->now + RETRY_MS;
}

int dns_resolve(struct net *net, const char *name)
{
    struct dns_client *dns = &net->dns;
    dns->state = DNS_FAILED;
    if (net->state != NET_READY || net->dns_server == 0)
        return 0;

    uint8_t *q = dns->query;
    __builtin_memset(q, 0, HEADER_SIZE);
    int encoded = encode_name(name, q + HEADER_SIZE, DNS_QUERY_MAX - HEADER_SIZE - 4);
    if (encoded == 0)
        return 0;

    /* A random ID and source port make forged replies harder to land. */
    uint32_t random = net->driver.random(net->driver.context);
    dns->id = (uint16_t)random;
    dns->port = (uint16_t)(49152 + (random >> 16) % 16384);
    net_put16(q, dns->id);
    net_put16(q + 2, 0x0100); /* Recursion desired. */
    net_put16(q + 4, 1);
    net_put16(q + HEADER_SIZE + encoded, TYPE_A);
    net_put16(q + HEADER_SIZE + encoded + 2, CLASS_IN);
    dns->query_len = HEADER_SIZE + encoded + 4;
    dns->retries = 0;
    dns->state = DNS_WAITING;
    send_query(net);
    return 1;
}

/* Advances past a possibly compressed name. Returns the new offset, or -1 if malformed. */
static int skip_name(const uint8_t *p, int length, int offset)
{
    while (offset < length) {
        uint8_t label = p[offset];
        if (label == 0)
            return offset + 1;
        if ((label & 0xc0) == 0xc0)
            return offset + 2 <= length ? offset + 2 : -1;
        if (label & 0xc0)
            return -1;
        offset += label + 1;
    }
    return -1;
}

void dns_receive(struct net *net, uint32_t src, uint16_t src_port, uint16_t dst_port,
                 const uint8_t *p, int length)
{
    struct dns_client *dns = &net->dns;
    if (dns->state != DNS_WAITING || src != net->dns_server || src_port != DNS_PORT ||
        dst_port != dns->port || length < HEADER_SIZE)
        return;

    uint16_t flags = net_get16(p + 2);
    if (net_get16(p) != dns->id || !(flags & 0x8000) || (flags & 0x7800) ||
        net_get16(p + 4) != 1)
        return;

    /* The echoed question must match what was asked. */
    int question = dns->query_len - HEADER_SIZE;
    if (length < HEADER_SIZE + question)
        return;
    for (int i = 0; i < question; i++) {
        if (lower(p[HEADER_SIZE + i]) != lower(dns->query[HEADER_SIZE + i]))
            return;
    }

    if (flags & 0x0200) { /* Truncated replies are not retried over TCP. */
        dns->state = DNS_FAILED;
        return;
    }
    int rcode = flags & 15;
    if (rcode != 0) {
        dns->state = rcode == RCODE_NXDOMAIN ? DNS_NOT_FOUND : DNS_FAILED;
        return;
    }

    int offset = HEADER_SIZE + question;
    int answers = net_get16(p + 6);
    for (int i = 0; i < answers; i++) {
        offset = skip_name(p, length, offset);
        if (offset < 0 || offset + 10 > length) {
            dns->state = DNS_FAILED;
            return;
        }
        int type = net_get16(p + offset);
        int class_code = net_get16(p + offset + 2);
        int data_length = net_get16(p + offset + 8);
        offset += 10;
        if (data_length > length - offset) {
            dns->state = DNS_FAILED;
            return;
        }
        if (type == TYPE_A && class_code == CLASS_IN && data_length == 4) {
            dns->address = net_get32(p + offset);
            dns->state = DNS_DONE;
            return;
        }
        offset += data_length;
    }
    dns->state = DNS_NOT_FOUND;
}

void dns_poll(struct net *net)
{
    struct dns_client *dns = &net->dns;
    if (dns->state != DNS_WAITING || !net_time_reached(net->now, dns->deadline))
        return;
    if (++dns->retries > MAX_RETRIES)
        dns->state = DNS_FAILED;
    else
        send_query(net);
}
