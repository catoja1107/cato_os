/*
 * Host tests for the network stack, URL parser, HTTP client, and HTML renderer.
 * A scripted peer plays the router, DNS server, and web server. Built with ASan
 * and UBSan, and fuzzed because all of this code handles untrusted input.
 */
#include <stdio.h>
#include <string.h>

#include "html.h"
#include "http.h"
#include "net.h"
#include "text.h"
#include "url.h"

#define OUR_IP 0x0a00020fu
#define PEER_IP 0x0a000202u
#define DNS_IP 0x0a000203u
#define MASK 0xffffff00u
#define SERVER_ISN 0x10000000u
#define QUEUE 64
#define FLAG_FIN 0x01
#define FLAG_SYN 0x02
#define FLAG_RST 0x04
#define FLAG_ACK 0x10

static int failures;
static struct net g_net;
static struct http_client g_http;
static struct web_doc g_doc;
static uint8_t g_body[HTTP_BODY_MAX];

static const uint8_t OUR_MAC[6] = { 0x52, 0x54, 0x00, 0x12, 0x34, 0x56 };
static const uint8_t PEER_MAC[6] = { 0x52, 0x55, 0x0a, 0x00, 0x02, 0x02 };

static uint8_t g_queue[QUEUE][NET_FRAME_MAX];
static int g_queue_length[QUEUE];
static int g_queue_head;
static int g_queue_count;
static uint8_t g_last[NET_FRAME_MAX];
static int g_last_length;
static int g_sent;
static int g_driver_up;
static uint32_t g_now;
static uint32_t g_random = 0x12345678u;
static uint32_t g_peer_seq;
static uint32_t g_our_seq;
static uint16_t g_local_port;

#define CHECK(condition)                                                 \
    do {                                                                 \
        if (!(condition)) {                                              \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);  \
            failures++;                                                  \
        }                                                                \
    } while (0)

static uint32_t next_random(void)
{
    g_random ^= g_random << 13;
    g_random ^= g_random >> 17;
    g_random ^= g_random << 5;
    return g_random;
}

static int driver_start(void *context, uint8_t mac[6])
{
    (void)context;
    memcpy(mac, OUR_MAC, 6);
    return g_driver_up;
}

static int driver_send(void *context, const uint8_t *frame, int length)
{
    (void)context;
    memcpy(g_last, frame, (size_t)length);
    g_last_length = length;
    g_sent++;
    return 1;
}

static int driver_receive(void *context, uint8_t *frame, int capacity)
{
    (void)context;
    if (g_queue_count == 0)
        return 0;
    int length = g_queue_length[g_queue_head];
    if (length > capacity)
        length = capacity;
    memcpy(frame, g_queue[g_queue_head], (size_t)length);
    g_queue_head = (g_queue_head + 1) % QUEUE;
    g_queue_count--;
    return length;
}

static uint32_t driver_random(void *context)
{
    (void)context;
    return next_random();
}

static struct net *fresh_net(void)
{
    struct net_driver driver = {
        NULL, driver_start, driver_send, driver_receive, driver_random, NULL,
    };
    g_queue_head = 0;
    g_queue_count = 0;
    g_sent = 0;
    g_last_length = 0;
    g_driver_up = 1;
    g_now = 1000;
    net_init(&g_net, &driver);
    net_poll(&g_net, g_now);
    return &g_net;
}

static void deliver(struct net *net, const uint8_t *frame, int length)
{
    int slot = (g_queue_head + g_queue_count) % QUEUE;
    memcpy(g_queue[slot], frame, (size_t)length);
    g_queue_length[slot] = length;
    g_queue_count++;
    net_poll(net, g_now);
}

static void advance(struct net *net, uint32_t ms)
{
    g_now += ms;
    net_poll(net, g_now);
}

static int make_ip(uint8_t *frame, uint32_t src, uint32_t dst, int protocol, const uint8_t *data,
                   int length)
{
    memcpy(frame, OUR_MAC, 6);
    memcpy(frame + 6, PEER_MAC, 6);
    net_put16(frame + 12, 0x0800);

    uint8_t *ip = frame + 14;
    memset(ip, 0, 20);
    ip[0] = 0x45;
    net_put16(ip + 2, (uint16_t)(20 + length));
    ip[8] = 64;
    ip[9] = (uint8_t)protocol;
    net_put32(ip + 12, src);
    net_put32(ip + 16, dst);
    net_put16(ip + 10, net_checksum(ip, 20, 0));
    memcpy(ip + 20, data, (size_t)length);
    return 34 + length;
}

static int make_udp(uint8_t *frame, uint32_t src, uint32_t dst, uint16_t src_port,
                    uint16_t dst_port, const uint8_t *data, int length)
{
    uint8_t udp[NET_MTU];
    net_put16(udp, src_port);
    net_put16(udp + 2, dst_port);
    net_put16(udp + 4, (uint16_t)(8 + length));
    net_put16(udp + 6, 0);
    memcpy(udp + 8, data, (size_t)length);
    uint16_t sum = net_checksum(udp, 8 + length, net_pseudo_sum(src, dst, 17, 8 + length));
    net_put16(udp + 6, sum ? sum : 0xffff);
    return make_ip(frame, src, dst, 17, udp, 8 + length);
}

static void deliver_arp_reply(struct net *net, uint32_t ip)
{
    uint8_t frame[60] = { 0 };
    memcpy(frame, OUR_MAC, 6);
    memcpy(frame + 6, PEER_MAC, 6);
    net_put16(frame + 12, 0x0806);
    uint8_t *arp = frame + 14;
    net_put16(arp, 1);
    net_put16(arp + 2, 0x0800);
    arp[4] = 6;
    arp[5] = 4;
    net_put16(arp + 6, 2);
    memcpy(arp + 8, PEER_MAC, 6);
    net_put32(arp + 14, ip);
    memcpy(arp + 18, OUR_MAC, 6);
    net_put32(arp + 24, OUR_IP);
    deliver(net, frame, 60);
}

static int make_dhcp(uint8_t *packet, uint32_t xid, int type, uint32_t offered)
{
    memset(packet, 0, 300);
    packet[0] = 2;
    packet[1] = 1;
    packet[2] = 6;
    net_put32(packet + 4, xid);
    net_put32(packet + 16, offered);
    memcpy(packet + 28, OUR_MAC, 6);
    net_put32(packet + 236, 0x63825363u);

    int o = 240;
    packet[o++] = 53;
    packet[o++] = 1;
    packet[o++] = (uint8_t)type;
    packet[o++] = 54;
    packet[o++] = 4;
    net_put32(packet + o, PEER_IP);
    o += 4;
    packet[o++] = 1;
    packet[o++] = 4;
    net_put32(packet + o, MASK);
    o += 4;
    packet[o++] = 3;
    packet[o++] = 4;
    net_put32(packet + o, PEER_IP);
    o += 4;
    packet[o++] = 6;
    packet[o++] = 4;
    net_put32(packet + o, DNS_IP);
    o += 4;
    packet[o++] = 255;
    return 300;
}

static void deliver_dhcp(struct net *net, uint32_t xid, int type)
{
    uint8_t packet[300];
    uint8_t frame[NET_FRAME_MAX];
    int length = make_dhcp(packet, xid, type, OUR_IP);
    deliver(net, frame, make_udp(frame, PEER_IP, 0xffffffffu, 67, 68, packet, length));
}

static struct net *online_net(void)
{
    struct net *net = fresh_net();
    net_start(net);
    deliver_dhcp(net, net->dhcp.xid, 2);
    deliver_dhcp(net, net->dhcp.xid, 5);
    CHECK(net->state == NET_READY);
    deliver_arp_reply(net, PEER_IP);
    deliver_arp_reply(net, DNS_IP);
    return net;
}

static const uint8_t *last_tcp(void)
{
    return g_last + 34;
}

static void deliver_tcp(struct net *net, uint32_t seq, uint32_t ack, int flags, const void *data,
                        int length)
{
    uint8_t segment[NET_MTU];
    uint8_t frame[NET_FRAME_MAX];

    net_put16(segment, net->tcp.remote_port);
    net_put16(segment + 2, g_local_port);
    net_put32(segment + 4, seq);
    net_put32(segment + 8, ack);
    segment[12] = 5 << 4;
    segment[13] = (uint8_t)flags;
    net_put16(segment + 14, 8192);
    net_put16(segment + 16, 0);
    net_put16(segment + 18, 0);
    if (length > 0)
        memcpy(segment + 20, data, (size_t)length);
    net_put16(segment + 16, net_checksum(segment, 20 + length,
                                         net_pseudo_sum(PEER_IP, OUR_IP, 6, 20 + length)));
    deliver(net, frame, make_ip(frame, PEER_IP, OUR_IP, 6, segment, 20 + length));
}

static void accept_connection(struct net *net)
{
    CHECK(last_tcp()[13] == FLAG_SYN);
    g_our_seq = net_get32(last_tcp() + 4) + 1;
    g_local_port = net_get16(last_tcp());
    deliver_tcp(net, SERVER_ISN, g_our_seq, FLAG_SYN | FLAG_ACK, NULL, 0);
    g_peer_seq = SERVER_ISN + 1;
    CHECK(net->tcp.state == TCP_ESTABLISHED);
}

static void send_data(struct net *net, const void *data, int length)
{
    deliver_tcp(net, g_peer_seq, net->tcp.snd_nxt, FLAG_ACK, data, length);
    g_peer_seq += (uint32_t)length;
}

static void test_basics(void)
{
    /* The worked example from RFC 1071. */
    const uint8_t data[8] = { 0x00, 0x01, 0xf2, 0x03, 0xf4, 0xf5, 0xf6, 0xf7 };
    CHECK(net_checksum(data, 8, 0) == (uint16_t)~0xddf2);

    char text[20];
    uint32_t ip;
    CHECK(net_parse_ip("10.0.2.15", &ip) && ip == OUR_IP);
    net_format_ip(ip, text, sizeof text);
    CHECK(strcmp(text, "10.0.2.15") == 0);
    CHECK(!net_parse_ip("256.0.0.1", &ip));
    CHECK(!net_parse_ip("1.2.3", &ip));
    CHECK(!net_parse_ip("1.2.3.4.5", &ip));
    CHECK(!net_parse_ip("example.com", &ip));
}

static void test_no_traffic_until_started(void)
{
    struct net *net = fresh_net();
    advance(net, 60000);
    CHECK(g_sent == 0 && net->state == NET_OFF);

    g_driver_up = 0;
    net_start(net);
    CHECK(net->state == NET_FAILED && net->failure == NET_FAIL_NO_ADAPTER && g_sent == 0);
}

static void test_dhcp(void)
{
    struct net *net = fresh_net();
    net_start(net);
    CHECK(g_sent == 1 && net->state == NET_CONFIGURING);
    uint32_t xid = net->dhcp.xid;

    /* Replies for another transaction are ignored. */
    deliver_dhcp(net, xid + 1, 2);
    CHECK(net->dhcp.phase == DHCP_DISCOVER);

    deliver_dhcp(net, xid, 2);
    CHECK(net->dhcp.phase == DHCP_REQUEST);

    /* A NAK restarts the exchange. */
    deliver_dhcp(net, xid, 6);
    CHECK(net->dhcp.phase == DHCP_DISCOVER && net->dhcp.xid != xid);

    deliver_dhcp(net, net->dhcp.xid, 2);
    deliver_dhcp(net, net->dhcp.xid, 5);
    CHECK(net->state == NET_READY && net->ip == OUR_IP && net->mask == MASK);
    CHECK(net->router == PEER_IP && net->dns_server == DNS_IP);

    /* No server: retries, then a clear failure. */
    net = fresh_net();
    net_start(net);
    for (int i = 0; i < 8; i++)
        advance(net, 2100);
    CHECK(net->state == NET_FAILED && net->failure == NET_FAIL_NO_DHCP);
}

static void test_arp(void)
{
    struct net *net = online_net();
    uint8_t frame[60] = { 0 };

    /* A request for our address gets a reply to the sender. */
    memset(frame, 0xff, 6);
    memcpy(frame + 6, PEER_MAC, 6);
    net_put16(frame + 12, 0x0806);
    uint8_t *arp = frame + 14;
    net_put16(arp, 1);
    net_put16(arp + 2, 0x0800);
    arp[4] = 6;
    arp[5] = 4;
    net_put16(arp + 6, 1);
    memcpy(arp + 8, PEER_MAC, 6);
    net_put32(arp + 14, 0x0a000209u);
    net_put32(arp + 24, OUR_IP);

    int sent = g_sent;
    deliver(net, frame, 60);
    CHECK(g_sent == sent + 1);
    CHECK(net_get16(g_last + 20) == 2 && memcmp(g_last, PEER_MAC, 6) == 0);

    /* A request for someone else is ignored. */
    sent = g_sent;
    net_put32(arp + 24, 0x0a000299u);
    deliver(net, frame, 60);
    CHECK(g_sent == sent);

    /* A packet waiting on ARP is sent when the answer arrives. */
    net = fresh_net();
    net_start(net);
    deliver_dhcp(net, net->dhcp.xid, 2);
    deliver_dhcp(net, net->dhcp.xid, 5);
    CHECK(tcp_connect(net, PEER_IP, 80));
    CHECK(net_get16(g_last + 12) == 0x0806);
    deliver_arp_reply(net, PEER_IP);
    CHECK(net_get16(g_last + 12) == 0x0800 && last_tcp()[13] == FLAG_SYN);
}

static void test_bad_packets_dropped(void)
{
    struct net *net = online_net();
    uint8_t frame[NET_FRAME_MAX];
    uint8_t data[4] = { 1, 2, 3, 4 };
    CHECK(dns_resolve(net, "example.com"));

    int length = make_udp(frame, DNS_IP, OUR_IP, 53, net->dns.port, data, 4);
    int sent = g_sent;

    /* Bad IP checksum, bad UDP checksum, fragments, and truncation. */
    frame[24] ^= 0xff;
    deliver(net, frame, length);
    frame[24] ^= 0xff;
    frame[length - 1] ^= 0xff;
    deliver(net, frame, length);
    frame[length - 1] ^= 0xff;
    net_put16(frame + 20, 0x2000);
    net_put16(frame + 24, 0);
    net_put16(frame + 24, net_checksum(frame + 14, 20, 0));
    deliver(net, frame, length);
    deliver(net, frame, 20);
    deliver(net, frame, 5);

    /* Frames for another MAC address are ignored. */
    frame[0] ^= 1;
    deliver(net, frame, length);
    CHECK(net->dns.state == DNS_WAITING && g_sent == sent);
}

static int make_dns_reply(const struct net *net, uint8_t *out, int flags, int with_cname)
{
    const struct dns_client *dns = &net->dns;
    int question = dns->query_len - 12;
    memcpy(out, dns->query, (size_t)dns->query_len);
    net_put16(out + 2, (uint16_t)(0x8180 | flags));
    net_put16(out + 6, (uint16_t)(with_cname ? 2 : 1));

    int o = 12 + question;
    if (with_cname) {
        static const uint8_t CNAME[] = { 0xc0, 0x0c, 0, 5, 0, 1, 0, 0, 0, 60, 0, 4, 1, 'x', 0xc0, 0x0c };
        memcpy(out + o, CNAME, sizeof CNAME);
        o += (int)sizeof CNAME;
    }
    static const uint8_t ANSWER[] = { 0xc0, 0x0c, 0, 1, 0, 1, 0, 0, 0, 60, 0, 4, 93, 184, 216, 34 };
    memcpy(out + o, ANSWER, sizeof ANSWER);
    return o + (int)sizeof ANSWER;
}

static void test_dns(void)
{
    struct net *net = online_net();
    uint8_t reply[512];
    uint8_t frame[NET_FRAME_MAX];

    CHECK(!dns_resolve(net, "bad name.com"));
    CHECK(!dns_resolve(net, "a..b"));
    CHECK(dns_resolve(net, "Example.COM"));

    int length = make_dns_reply(net, reply, 0, 1);

    /* Replies from the wrong server, wrong port, or with the wrong ID are ignored. */
    deliver(net, frame, make_udp(frame, PEER_IP, OUR_IP, 53, net->dns.port, reply, length));
    CHECK(net->dns.state == DNS_WAITING);
    deliver(net, frame, make_udp(frame, DNS_IP, OUR_IP, 54, net->dns.port, reply, length));
    CHECK(net->dns.state == DNS_WAITING);
    deliver(net, frame,
            make_udp(frame, DNS_IP, OUR_IP, 53, (uint16_t)(net->dns.port + 1), reply, length));
    CHECK(net->dns.state == DNS_WAITING);
    reply[0] ^= 0xff;
    deliver(net, frame, make_udp(frame, DNS_IP, OUR_IP, 53, net->dns.port, reply, length));
    CHECK(net->dns.state == DNS_WAITING);
    reply[0] ^= 0xff;

    /* A reply for a different question is ignored. */
    reply[13] ^= 1;
    deliver(net, frame, make_udp(frame, DNS_IP, OUR_IP, 53, net->dns.port, reply, length));
    CHECK(net->dns.state == DNS_WAITING);
    reply[13] ^= 1;

    deliver(net, frame, make_udp(frame, DNS_IP, OUR_IP, 53, net->dns.port, reply, length));
    CHECK(net->dns.state == DNS_DONE && net->dns.address == 0x5db8d822u);

    CHECK(dns_resolve(net, "missing.example"));
    length = make_dns_reply(net, reply, 3, 0);
    deliver(net, frame, make_udp(frame, DNS_IP, OUR_IP, 53, net->dns.port, reply, length));
    CHECK(net->dns.state == DNS_NOT_FOUND);

    /* A timeout retries and then fails. */
    CHECK(dns_resolve(net, "slow.example"));
    int sent = g_sent;
    for (int i = 0; i < 5; i++)
        advance(net, 2100);
    CHECK(net->dns.state == DNS_FAILED && g_sent == sent + 3);
}

static void test_tcp(void)
{
    struct net *net = online_net();
    uint8_t out[64];

    CHECK(tcp_connect(net, PEER_IP, 80));
    accept_connection(net);
    CHECK(tcp_is_established(net));

    /* Data sent to the server, then retransmitted if unacknowledged. */
    CHECK(tcp_send(net, (const uint8_t *)"hello", 5));
    CHECK(memcmp(g_last + 54, "hello", 5) == 0);
    int sent = g_sent;
    advance(net, 1100);
    CHECK(g_sent == sent + 1 && memcmp(g_last + 54, "hello", 5) == 0);
    deliver_tcp(net, g_peer_seq, g_our_seq + 5, FLAG_ACK, NULL, 0);
    sent = g_sent;
    advance(net, 10000);
    CHECK(g_sent == sent);

    /* In-order data. */
    send_data(net, "abc", 3);
    CHECK(tcp_read(net, out, sizeof out) == 3 && memcmp(out, "abc", 3) == 0);
    CHECK(net_get32(last_tcp() + 8) == g_peer_seq);

    /* Out-of-order data is not delivered, and is acknowledged at the old position. */
    deliver_tcp(net, g_peer_seq + 10, g_our_seq + 5, FLAG_ACK, "later", 5);
    CHECK(tcp_read(net, out, sizeof out) == 0);
    CHECK(net_get32(last_tcp() + 8) == g_peer_seq);

    /* A retransmission overlapping old data delivers only the new bytes. */
    deliver_tcp(net, g_peer_seq - 2, g_our_seq + 5, FLAG_ACK, "bcNEW", 5);
    g_peer_seq += 3;
    CHECK(tcp_read(net, out, sizeof out) == 3 && memcmp(out, "NEW", 3) == 0);

    /* A fully old segment is just acknowledged. */
    deliver_tcp(net, g_peer_seq - 3, g_our_seq + 5, FLAG_ACK, "NEW", 3);
    CHECK(tcp_read(net, out, sizeof out) == 0);

    /* A reset with the wrong sequence number is ignored. */
    deliver_tcp(net, g_peer_seq + 100, 0, FLAG_RST, NULL, 0);
    CHECK(tcp_is_established(net));

    /* The server closes, and then we close. */
    send_data(net, "bye", 3);
    deliver_tcp(net, g_peer_seq, g_our_seq + 5, FLAG_ACK | FLAG_FIN, NULL, 0);
    g_peer_seq++;
    CHECK(net->tcp.state == TCP_CLOSE_WAIT && !tcp_is_finished(net));
    CHECK(tcp_read(net, out, sizeof out) == 3 && tcp_is_finished(net));
    tcp_close(net);
    CHECK(net->tcp.state == TCP_LAST_ACK && (last_tcp()[13] & FLAG_FIN));
    deliver_tcp(net, g_peer_seq, g_our_seq + 6, FLAG_ACK, NULL, 0);
    CHECK(net->tcp.state == TCP_CLOSED && net->tcp.error == TCP_OK);
}

static void test_tcp_failures(void)
{
    struct net *net = online_net();

    /* Connection refused. */
    CHECK(tcp_connect(net, PEER_IP, 81));
    g_our_seq = net_get32(last_tcp() + 4) + 1;
    g_local_port = net_get16(last_tcp());
    deliver_tcp(net, 0, g_our_seq + 5, FLAG_RST | FLAG_ACK, NULL, 0);
    CHECK(net->tcp.state == TCP_SYN_SENT); /* A forged reset with a bad ACK is ignored. */
    deliver_tcp(net, 0, g_our_seq, FLAG_RST | FLAG_ACK, NULL, 0);
    CHECK(net->tcp.state == TCP_CLOSED && net->tcp.error == TCP_ERR_REFUSED);

    /* A SYN-ACK with the wrong acknowledgment does not establish a connection. */
    CHECK(tcp_connect(net, PEER_IP, 80));
    g_our_seq = net_get32(last_tcp() + 4) + 1;
    g_local_port = net_get16(last_tcp());
    deliver_tcp(net, SERVER_ISN, g_our_seq + 1, FLAG_SYN | FLAG_ACK, NULL, 0);
    CHECK(net->tcp.state == TCP_SYN_SENT);

    /* No answer: retries back off, then time out. */
    for (int i = 0; i < 10; i++)
        advance(net, 9000);
    CHECK(net->tcp.state == TCP_CLOSED && net->tcp.error == TCP_ERR_TIMEOUT);

    /* That wait outlived the ARP cache, so the peer must be learned again. */
    deliver_arp_reply(net, PEER_IP);

    /* An in-sequence reset closes an open connection. */
    CHECK(tcp_connect(net, PEER_IP, 80));
    accept_connection(net);
    deliver_tcp(net, g_peer_seq, 0, FLAG_RST, NULL, 0);
    CHECK(net->tcp.state == TCP_CLOSED && net->tcp.error == TCP_ERR_RESET);

    /* The receive buffer never overflows. */
    CHECK(tcp_connect(net, PEER_IP, 80));
    accept_connection(net);
    static uint8_t block[1460];
    for (int i = 0; i < 100; i++) {
        deliver_tcp(net, g_peer_seq, net->tcp.snd_nxt, FLAG_ACK, block, sizeof block);
        g_peer_seq = net->tcp.rcv_nxt;
    }
    CHECK(net->tcp.rx_len == TCP_RX_SIZE);
    CHECK(net_get16(last_tcp() + 14) == 0);
    static uint8_t drain[TCP_RX_SIZE];
    CHECK(tcp_read(net, drain, sizeof drain) == TCP_RX_SIZE);
    CHECK(net_get16(last_tcp() + 14) > 0); /* The window reopened. */
}

static void start_http(struct net *net, const char *address)
{
    struct url url;
    CHECK(url_parse(address, &url));
    http_start(&g_http, net, &url, g_body, sizeof g_body);
    CHECK(g_http.phase == HTTP_CONNECTING);
    accept_connection(net);
    http_poll(&g_http);
    CHECK(g_http.phase == HTTP_HEADERS);
}

static void respond(struct net *net, const char *text)
{
    int length = (int)strlen(text);
    for (int offset = 0; offset < length; offset += 1000) {
        int count = length - offset < 1000 ? length - offset : 1000;
        send_data(net, text + offset, count);
        http_poll(&g_http);
    }
}

static void test_http(void)
{
    struct net *net = online_net();

    start_http(net, "http://10.0.2.2:8080/a?b=1");
    /* The request is well formed and carries no identifying client name. */
    g_last[g_last_length < NET_FRAME_MAX ? g_last_length : NET_FRAME_MAX - 1] = 0;
    CHECK(strstr((const char *)g_last + 54, "GET /a?b=1 HTTP/1.1\r\n") != NULL);
    CHECK(strstr((const char *)g_last + 54, "Host: 10.0.2.2:8080\r\n") != NULL);
    CHECK(strstr((const char *)g_last + 54, "Cato") == NULL);

    respond(net, "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Length: 5\r\n\r\nhello");
    CHECK(g_http.phase == HTTP_DONE && g_http.status == 200);
    CHECK(g_http.body_length == 5 && memcmp(g_body, "hello", 5) == 0 && !g_http.truncated);
    CHECK(strcmp(g_http.content_type, "text/html") == 0);

    /* Headers split across segments, in mixed case, with chunked encoding. */
    CHECK(tcp_connect(net, PEER_IP, 80));
    start_http(net, "http://10.0.2.2/");
    send_data(net, "HTTP/1.1 200 OK\r\nTransfer-Enc", 29);
    http_poll(&g_http);
    send_data(net, "oding: CHUNKED\r\n\r\n5;x=1\r\nhel", 28);
    http_poll(&g_http);
    send_data(net, "lo\r\n7\r\n, world\r\n0\r\n\r\n", 21);
    http_poll(&g_http);
    CHECK(g_http.phase == HTTP_DONE && g_http.body_length == 12);
    CHECK(memcmp(g_body, "hello, world", 12) == 0 && !g_http.truncated);

    /* A redirect exposes its location. */
    start_http(net, "http://10.0.2.2/old");
    respond(net, "HTTP/1.1 302 Found\r\nLocation: /new\r\nContent-Length: 0\r\n\r\n");
    CHECK(g_http.phase == HTTP_DONE && g_http.status == 302);
    CHECK(strcmp(g_http.location, "/new") == 0);

    /* The end of the connection ends a body with no declared length. */
    start_http(net, "http://10.0.2.2/");
    respond(net, "HTTP/1.0 200 OK\r\n\r\nstream");
    CHECK(g_http.phase == HTTP_HEADERS || g_http.phase == HTTP_BODY);
    deliver_tcp(net, g_peer_seq, net->tcp.snd_nxt, FLAG_ACK | FLAG_FIN, NULL, 0);
    http_poll(&g_http);
    CHECK(g_http.phase == HTTP_DONE && g_http.body_length == 6 && !g_http.truncated);

    /* A body that ends early is reported as truncated. */
    start_http(net, "http://10.0.2.2/");
    respond(net, "HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\nshort");
    deliver_tcp(net, g_peer_seq, net->tcp.snd_nxt, FLAG_ACK | FLAG_FIN, NULL, 0);
    http_poll(&g_http);
    CHECK(g_http.phase == HTTP_DONE && g_http.truncated);

    /* Compressed responses, junk, and oversized headers fail cleanly. */
    start_http(net, "http://10.0.2.2/");
    respond(net, "HTTP/1.1 200 OK\r\nContent-Encoding: gzip\r\n\r\nxx");
    CHECK(g_http.phase == HTTP_FAILED);

    start_http(net, "http://10.0.2.2/");
    respond(net, "this is not http\r\n\r\n");
    CHECK(g_http.phase == HTTP_FAILED);

    start_http(net, "http://10.0.2.2/");
    static char huge[HTTP_HEAD_MAX + 100];
    memset(huge, 'x', sizeof huge - 1);
    respond(net, huge);
    CHECK(g_http.phase == HTTP_FAILED);

    /* Invalid chunk framing stops the transfer. */
    start_http(net, "http://10.0.2.2/");
    respond(net, "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nZZ\r\n");
    CHECK(g_http.phase == HTTP_DONE && g_http.truncated);

    /* A server that goes silent times out. */
    start_http(net, "http://10.0.2.2/");
    advance(net, 21000);
    http_poll(&g_http);
    CHECK(g_http.phase == HTTP_FAILED);
}

static void test_http_body_limit(void)
{
    struct net *net = online_net();
    static uint8_t small[32];
    struct url url;

    CHECK(url_parse("http://10.0.2.2/", &url));
    http_start(&g_http, net, &url, small, sizeof small);
    accept_connection(net);
    http_poll(&g_http);
    respond(net, "HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\n"
                 "0123456789012345678901234567890123456789");
    CHECK(g_http.phase == HTTP_DONE && g_http.truncated && g_http.body_length == 32);
}

static void check_url(const char *input, const char *expected)
{
    struct url url;
    char text[URL_MAX];
    if (!url_parse(input, &url)) {
        CHECK(expected == NULL);
        return;
    }
    url_format(&url, text, sizeof text);
    if (!expected || strcmp(text, expected) != 0) {
        printf("FAIL url '%s' gave '%s'\n", input, text);
        failures++;
    }
}

static void check_resolve(const char *base_text, const char *reference, const char *expected)
{
    struct url base, out;
    char text[URL_MAX];
    CHECK(url_parse(base_text, &base));
    CHECK(url_resolve(&base, reference, &out));
    url_format(&out, text, sizeof text);
    if (strcmp(text, expected) != 0) {
        printf("FAIL resolve '%s' + '%s' gave '%s'\n", base_text, reference, text);
        failures++;
    }
}

static void test_urls(void)
{
    struct url url;

    check_url("example.com", "http://example.com/");
    check_url("EXAMPLE.com:8080/Path?q=1#frag", "http://example.com:8080/Path?q=1");
    check_url("http://a.b/c d", "http://a.b/c%20d");
    check_url("http://a.b:80/", "http://a.b/");
    check_url("https://a.b/x", "https://a.b/x");
    check_url("  http://a.b/  ", "http://a.b/");
    check_url("http://a.b/x/../y/./z", "http://a.b/y/z");
    check_url("http://a.b/../../..", "http://a.b/");
    check_url("about:home", "about:home");

    /* Spoofing, header injection, and malformed input are rejected. */
    check_url("http://user@evil.com/", NULL);
    check_url("http://a.b/\r\nX-Evil: 1", NULL);
    check_url("http://a.b:99999/", NULL);
    check_url("http://a.b:0/", NULL);
    check_url("http://[::1]/", NULL);
    check_url("http:///path", NULL);
    check_url("", NULL);

    CHECK(url_parse("javascript:alert(1)", &url) && url.scheme == URL_OTHER);
    CHECK(url_parse("mailto:a@b.c", &url) && url.scheme == URL_OTHER);
    CHECK(url_parse("ftp://a.b/", &url) && url.scheme == URL_OTHER);

    check_resolve("http://a.b/c/d.html", "e.html", "http://a.b/c/e.html");
    check_resolve("http://a.b/c/d.html", "../e", "http://a.b/e");
    check_resolve("http://a.b/c/d.html", "/x", "http://a.b/x");
    check_resolve("http://a.b/c/d.html?q=1", "?r=2", "http://a.b/c/d.html?r=2");
    check_resolve("http://a.b/c/d.html", "#top", "http://a.b/c/d.html");
    check_resolve("http://a.b:81/c/", "e", "http://a.b:81/c/e");
    check_resolve("http://a.b/c", "//x.y/z", "http://x.y/z");
    check_resolve("http://a.b/c", "http://x.y/", "http://x.y/");
    check_resolve("http://a.b/c", "https://x.y/", "https://x.y/");
}

static void parse(const char *html)
{
    struct url base;
    CHECK(url_parse("http://site.test/dir/page.html", &base));
    web_doc_clear(&g_doc);
    web_parse_html(&g_doc, &base, html, (int)strlen(html));
    web_layout(&g_doc, 40);
}

/* Rebuilds the visible text with one space between runs on a line. */
static void doc_text(char *out, int capacity)
{
    int o = 0;
    int line = 0;
    for (int i = 0; i < g_doc.run_count && o + 2 < capacity; i++) {
        const struct web_run *run = &g_doc.runs[i];
        if (run->style & WEB_RULE)
            continue;
        if (i > 0 && run->line != line)
            out[o++] = '\n';
        else if (i > 0)
            out[o++] = ' ';
        line = run->line;
        for (int k = 0; k < run->length && o + 1 < capacity; k++)
            out[o++] = g_doc.text[run->text + (uint32_t)k];
    }
    out[o] = 0;
}

static void test_html(void)
{
    char text[1024];

    parse("<!doctype html><html><head><title>  My &amp;\n Page </title>"
          "<style>body{color:red}</style><script>alert('<p>x</p>')</script></head>"
          "<body><!-- hidden --><h1>Hello</h1><p>One   two\nthree</p></body></html>");
    CHECK(strcmp(g_doc.title, "My & Page") == 0);
    doc_text(text, sizeof text);
    CHECK(strstr(text, "Hello") != NULL && strstr(text, "One two three") != NULL);
    CHECK(strstr(text, "alert") == NULL && strstr(text, "color") == NULL);
    CHECK(strstr(text, "hidden") == NULL);

    parse("<p>a &lt;b&gt; &#65;&#x42; caf&eacute; caf\xc3\xa9 \xe2\x80\x94 &bogus; &</p>");
    doc_text(text, sizeof text);
    CHECK(strstr(text, "a <b> AB") != NULL);
    CHECK(strstr(text, "cafe -") != NULL);
    CHECK(strstr(text, "&bogus;") != NULL);

    parse("<a href='next.html'>rel</a> <a href=\"/root?a=1&amp;b=2\">abs</a> "
          "<a href=\"javascript:alert(1)\">js</a> <a href=\"https://s.test/\">sec</a>");
    CHECK(g_doc.link_count == 3);
    CHECK(strcmp(web_link_url(&g_doc, 0), "http://site.test/dir/next.html") == 0);
    CHECK(strcmp(web_link_url(&g_doc, 1), "http://site.test/root?a=1&b=2") == 0);
    CHECK(strcmp(web_link_url(&g_doc, 2), "https://s.test/") == 0);

    parse("<ul><li>one</li><li>two</li></ul><ol><li>first</li><li>second</li></ol>");
    doc_text(text, sizeof text);
    CHECK(strstr(text, "* one") != NULL && strstr(text, "2. second") != NULL);

    parse("<pre>a  b\n\n  c</pre>");
    doc_text(text, sizeof text);
    CHECK(strstr(text, "a  b") != NULL && strstr(text, "  c") != NULL);

    parse("<img src=x alt='a cat'><img src=y>");
    doc_text(text, sizeof text);
    CHECK(strstr(text, "[image: a cat]") != NULL && strstr(text, "[image]") != NULL);

    /* Long words wrap, and no run exceeds the page width. */
    static char long_word[300];
    memset(long_word, 'w', sizeof long_word - 1);
    parse(long_word);
    CHECK(g_doc.line_count >= 7);
    for (int i = 0; i < g_doc.run_count; i++)
        CHECK(g_doc.runs[i].column + web_run_width(&g_doc.runs[i]) <= 40);

    /* Top-level headings are drawn double size and wrap within the width. */
    parse("<h1>A big heading that has to wrap across lines</h1><p>after</p>");
    CHECK(g_doc.runs[0].style & WEB_BIG);
    for (int i = 0; i < g_doc.run_count; i++)
        CHECK(g_doc.runs[i].column + web_run_width(&g_doc.runs[i]) <= 40);
    CHECK(g_doc.runs[1].line == g_doc.runs[0].line + 2 || g_doc.runs[1].line == g_doc.runs[0].line);

    parse("<blockquote>quoted</blockquote>");
    CHECK(g_doc.runs[0].style & WEB_QUOTE);

    /* Broken markup ends safely. */
    parse("<a href=\"unterminated");
    parse("<script>never closed");
    parse("<!-- never closed");
    parse("</a></b></ul></h1></pre></blockquote><");
    parse("<<<>>>&&&;;;");

    web_doc_clear(&g_doc);
    web_parse_text(&g_doc, "plain <b>text</b>\nline 2", 24);
    web_layout(&g_doc, 40);
    doc_text(text, sizeof text);
    CHECK(strstr(text, "<b>text</b>") != NULL);
}

static void test_html_limits(void)
{
    static char html[HTTP_BODY_MAX];
    struct url base;
    CHECK(url_parse("http://site.test/", &base));

    /* More links than the table holds. */
    int length = 0;
    while (length + 32 < (int)sizeof html) {
        memcpy(html + length, "<a href=\"/page\">x</a> ", 22);
        length += 22;
    }
    web_doc_clear(&g_doc);
    web_parse_html(&g_doc, &base, html, length);
    web_layout(&g_doc, 8);
    CHECK(g_doc.link_count <= WEB_LINKS_MAX);

    /* More words than the item table holds. */
    for (int i = 0; i + 1 < (int)sizeof html; i += 2) {
        html[i] = 'a';
        html[i + 1] = ' ';
    }
    web_doc_clear(&g_doc);
    web_parse_html(&g_doc, &base, html, sizeof html);
    web_layout(&g_doc, 8);
    CHECK(g_doc.truncated && g_doc.item_count <= WEB_ITEMS_MAX);

    /* Deeply nested lists keep the indent bounded. */
    length = 0;
    for (int i = 0; i < 5000; i++) {
        memcpy(html + length, "<ul><li>", 8);
        length += 8;
    }
    web_doc_clear(&g_doc);
    web_parse_html(&g_doc, &base, html, length);
    web_layout(&g_doc, 10);
    for (int i = 0; i < g_doc.run_count; i++)
        CHECK(g_doc.runs[i].column + web_run_width(&g_doc.runs[i]) <= 10);
}

static void test_fuzz_parsers(void)
{
    static const char SAMPLE[] =
        "<!doctype html><title>t&amp;x</title><h1 a=b>h</h1><p>t &lt; x &#65; \xc3\xa9</p>"
        "<a href='/a?b=c&amp;d'>l</a><ul><li>x<ol><li>y</ol></ul><pre>p\n q</pre><hr>"
        "<img alt=\"z\" src=q><table><tr><td>1<td>2</table><script>s</script><!-- c -->";
    static char buffer[sizeof SAMPLE];
    struct url base, out;
    char text[URL_MAX];
    CHECK(url_parse("http://f.test/d/p", &base));

    for (int round = 0; round < 4000; round++) {
        memcpy(buffer, SAMPLE, sizeof SAMPLE - 1);
        int edits = 1 + round % 12;
        for (int e = 0; e < edits; e++)
            buffer[next_random() % (sizeof SAMPLE - 1)] = (char)next_random();
        int length = (int)(next_random() % sizeof SAMPLE);
        web_doc_clear(&g_doc);
        web_parse_html(&g_doc, &base, buffer, length);
        int columns = 8 + (int)(next_random() % 120);
        web_layout(&g_doc, columns);
        for (int i = 0; i < g_doc.run_count; i++) {
            const struct web_run *run = &g_doc.runs[i];
            if (run->column + web_run_width(run) > columns ||
                run->text + run->length > WEB_TEXT_MAX) {
                CHECK(!"run out of bounds");
                break;
            }
        }

        char reference[96];
        int size = (int)(next_random() % (sizeof reference - 1));
        for (int i = 0; i < size; i++)
            reference[i] = (char)(1 + next_random() % 255);
        reference[size] = 0;
        if (url_parse(reference, &out))
            url_format(&out, text, sizeof text);
        if (url_resolve(&base, reference, &out))
            url_format(&out, text, sizeof text);
    }
}

static void test_fuzz_network(void)
{
    struct net *net = online_net();
    static uint8_t frame[NET_FRAME_MAX];
    uint8_t packet[300];

    for (int round = 0; round < 30000; round++) {
        int length = 0;
        switch (next_random() % 5) {
        case 0:
            length = make_dhcp(packet, net->dhcp.xid, 1 + (int)(next_random() % 6), OUR_IP);
            length = make_udp(frame, PEER_IP, OUR_IP, 67, 68, packet, length);
            break;
        case 1: {
            uint8_t data[64];
            for (int i = 0; i < 64; i++)
                data[i] = (uint8_t)next_random();
            length = make_udp(frame, DNS_IP, OUR_IP, 53, net->dns.port, data, 64);
            break;
        }
        case 2: {
            uint8_t data[64];
            for (int i = 0; i < 64; i++)
                data[i] = (uint8_t)next_random();
            net_put16(data, net->tcp.remote_port);
            net_put16(data + 2, net->tcp.local_port);
            net_put16(data + 16, 0);
            net_put16(data + 16, net_checksum(data, 64, net_pseudo_sum(PEER_IP, OUR_IP, 6, 64)));
            length = make_ip(frame, PEER_IP, OUR_IP, 6, data, 64);
            break;
        }
        case 3:
            length = 14 + (int)(next_random() % (NET_FRAME_MAX - 14));
            for (int i = 0; i < length; i++)
                frame[i] = (uint8_t)next_random();
            memcpy(frame, OUR_MAC, 6);
            net_put16(frame + 12, next_random() & 1 ? 0x0800 : 0x0806);
            break;
        default: {
            uint8_t data[32] = { 8, 0 };
            length = make_ip(frame, PEER_IP, OUR_IP, 1, data, 32);
            frame[14 + 20 + (next_random() % 32)] = (uint8_t)next_random();
            break;
        }
        }
        if (next_random() % 4 == 0)
            frame[next_random() % (unsigned)length] ^= (uint8_t)(1 << (next_random() % 8));
        deliver(net, frame, length);

        if (round % 500 == 0) {
            uint8_t sink[256];
            tcp_read(net, sink, sizeof sink);
            advance(net, next_random() % 3000);
            if (net->state != NET_READY)
                net = online_net();
            if (net->tcp.state == TCP_CLOSED)
                tcp_connect(net, PEER_IP, 80);
            if (net->dns.state != DNS_WAITING)
                dns_resolve(net, "fuzz.example");
        }
        CHECK(net->tcp.rx_len >= 0 && net->tcp.rx_len <= TCP_RX_SIZE);
    }
}

static void test_fuzz_http(void)
{
    static const char RESPONSE[] =
        "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nTransfer-Encoding: chunked\r\n"
        "Location: /x\r\nContent-Length: 12\r\n\r\n5\r\nhello\r\n7\r\n, world\r\n0\r\n\r\n";
    static char buffer[sizeof RESPONSE];
    struct net *net = online_net();

    for (int round = 0; round < 3000; round++) {
        start_http(net, "http://10.0.2.2/");
        memcpy(buffer, RESPONSE, sizeof RESPONSE - 1);
        int edits = (int)(next_random() % 8);
        for (int e = 0; e < edits; e++)
            buffer[next_random() % (sizeof RESPONSE - 1)] = (char)next_random();

        int offset = 0;
        int total = (int)sizeof RESPONSE - 1;
        while (offset < total && (g_http.phase == HTTP_HEADERS || g_http.phase == HTTP_BODY)) {
            int count = 1 + (int)(next_random() % 40);
            if (count > total - offset)
                count = total - offset;
            send_data(net, buffer + offset, count);
            http_poll(&g_http);
            offset += count;
        }
        CHECK(g_http.body_length >= 0 && g_http.body_length <= g_http.body_capacity);
        http_cancel(&g_http);
    }
}

int main(void)
{
    test_basics();
    test_no_traffic_until_started();
    test_dhcp();
    test_arp();
    test_bad_packets_dropped();
    test_dns();
    test_tcp();
    test_tcp_failures();
    test_http();
    test_http_body_limit();
    test_urls();
    test_html();
    test_html_limits();
    test_fuzz_parsers();
    test_fuzz_network();
    test_fuzz_http();

    if (failures) {
        printf("%d network check(s) failed\n", failures);
        return 1;
    }
    puts("network unit tests passed");
    return 0;
}
