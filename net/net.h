/*
 * Polled TCP/IP stack: Ethernet, ARP, IPv4, ICMP echo, UDP, DHCP, DNS, and one
 * TCP client connection. It has no firmware dependencies. A driver supplies raw
 * Ethernet frames, and the caller runs net_poll() regularly.
 *
 * Protocol references: RFC 826 (ARP), 791 (IP), 792 (ICMP), 768 (UDP),
 * 2131/2132 (DHCP), 1035 (DNS), and 9293 (TCP).
 */
#ifndef CATO_NET_H
#define CATO_NET_H

#include <stdint.h>

#define NET_MTU 1500
#define NET_FRAME_MAX 1514
#define NET_ARP_ENTRIES 16
#define TCP_RX_SIZE 65536
#define TCP_TX_SIZE 16384 /* Acknowledged bytes are released, so this is a sliding buffer. */
#define TCP_WINDOW 32768
#define TCP_MSS 1460
#define DNS_QUERY_MAX 288

enum net_state { NET_OFF, NET_CONFIGURING, NET_READY, NET_FAILED };
enum net_failure { NET_FAIL_NONE, NET_FAIL_NO_ADAPTER, NET_FAIL_NO_DHCP };
enum dhcp_phase { DHCP_IDLE, DHCP_DISCOVER, DHCP_REQUEST, DHCP_BOUND };
enum dns_state { DNS_IDLE, DNS_WAITING, DNS_DONE, DNS_NOT_FOUND, DNS_FAILED };
enum ping_state { PING_IDLE, PING_WAITING, PING_DONE, PING_FAILED };

enum tcp_state {
    TCP_CLOSED,
    TCP_SYN_SENT,
    TCP_ESTABLISHED,
    TCP_FIN_WAIT_1,
    TCP_FIN_WAIT_2,
    TCP_CLOSE_WAIT,
    TCP_LAST_ACK,
};

enum tcp_error { TCP_OK, TCP_ERR_REFUSED, TCP_ERR_RESET, TCP_ERR_TIMEOUT };

struct net_driver {
    void *context;
    /* Brings the adapter up on first use and fills in mac. Returns 1 on success. */
    int (*start)(void *context, uint8_t mac[6]);
    int (*send)(void *context, const uint8_t *frame, int length);
    /* Returns the frame length, or 0 if nothing is waiting. */
    int (*receive)(void *context, uint8_t *frame, int capacity);
    /* Fast random numbers for IDs and ports. Not for keys. */
    uint32_t (*random)(void *context);
    /* Fills out from a strong hardware source. Returns 0 if none exists. Optional. */
    int (*entropy)(void *context, uint8_t *out, int length);
};

struct arp_entry {
    uint32_t ip;
    uint32_t expires;
    uint8_t mac[6];
    uint8_t valid;
};

struct dhcp_client {
    int phase;
    int retries;
    uint32_t xid;
    uint32_t server;
    uint32_t offered;
    uint32_t deadline;
};

struct dns_client {
    int state;
    int retries;
    int query_len;
    uint16_t id;
    uint16_t port;
    uint32_t address;
    uint32_t deadline;
    uint8_t query[DNS_QUERY_MAX];
};

struct ping_client {
    int state;
    uint16_t id;
    uint16_t sequence;
    uint32_t target;
    uint32_t sent_at;
    uint32_t round_trip;
    uint32_t deadline;
};

struct tcp_conn {
    int state;
    int error;
    int fin_received;
    int fin_sent;
    int mss;
    int retries;
    int tx_len;
    int rx_head;
    int rx_len;
    uint32_t remote_ip;
    uint16_t remote_port;
    uint16_t local_port;
    uint32_t iss;
    uint32_t tx_base;
    uint32_t snd_una;
    uint32_t snd_nxt;
    uint32_t snd_wnd;
    uint32_t rcv_nxt;
    uint32_t fin_seq;
    uint32_t rto;
    uint32_t deadline;
    uint8_t tx[TCP_TX_SIZE];
    uint8_t rx[TCP_RX_SIZE];
};

struct net {
    struct net_driver driver;
    int state;
    int failure;
    uint32_t now;
    uint8_t mac[6];
    uint32_t ip;
    uint32_t mask;
    uint32_t router;
    uint32_t dns_server;
    uint16_t ip_id;
    /* UTC date for certificate checks: days since January 1st of year 0, 0 if unknown. */
    uint32_t clock_days;
    uint32_t clock_seconds;
    uint32_t rx_frames;
    uint32_t tx_frames;

    struct arp_entry arp[NET_ARP_ENTRIES];
    uint32_t pending_hop;
    int pending_len;
    uint8_t pending[NET_MTU];

    struct dhcp_client dhcp;
    struct dns_client dns;
    struct ping_client ping;
    struct tcp_conn tcp;

    uint8_t rx[NET_FRAME_MAX];
    uint8_t tx[NET_FRAME_MAX];
    uint8_t scratch[NET_MTU];
};

static inline uint16_t net_get16(const uint8_t *p)
{
    return (uint16_t)((p[0] << 8) | p[1]);
}

static inline uint32_t net_get32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static inline void net_put16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)(value >> 8);
    p[1] = (uint8_t)value;
}

static inline void net_put32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)(value >> 24);
    p[1] = (uint8_t)(value >> 16);
    p[2] = (uint8_t)(value >> 8);
    p[3] = (uint8_t)value;
}

static inline int net_time_reached(uint32_t now, uint32_t deadline)
{
    return (int32_t)(now - deadline) >= 0;
}

/* Stack */
void net_init(struct net *net, const struct net_driver *driver);
/* Starts the adapter and DHCP. Nothing is transmitted before the first call. */
void net_start(struct net *net);
void net_poll(struct net *net, uint32_t now_ms);
const char *net_state_text(const struct net *net);
void net_format_ip(uint32_t ip, char *out, int capacity);
int net_parse_ip(const char *text, uint32_t *ip);
uint16_t net_checksum(const uint8_t *data, int length, uint32_t seed);
uint32_t net_pseudo_sum(uint32_t src, uint32_t dst, uint8_t protocol, int length);
int net_ip_send(struct net *net, uint32_t dst, uint8_t protocol, const uint8_t *data, int length);
int net_udp_send(struct net *net, uint32_t dst, uint16_t src_port, uint16_t dst_port,
                 const uint8_t *data, int length);
int net_ping(struct net *net, uint32_t target);

/* DHCP */
void dhcp_start(struct net *net);
void dhcp_receive(struct net *net, const uint8_t *data, int length);
void dhcp_poll(struct net *net);

/* DNS: one query at a time, for IPv4 addresses. */
int dns_resolve(struct net *net, const char *name);
void dns_receive(struct net *net, uint32_t src, uint16_t src_port, uint16_t dst_port,
                 const uint8_t *data, int length);
void dns_poll(struct net *net);

/* TCP: one client connection at a time. */
int tcp_connect(struct net *net, uint32_t ip, uint16_t port);
/* Queues all of data, or nothing. Returns 1 if queued. */
int tcp_send(struct net *net, const uint8_t *data, int length);
/* Queues as much of data as fits. Returns the number of bytes queued. */
int tcp_write(struct net *net, const uint8_t *data, int length);
int tcp_read(struct net *net, uint8_t *out, int capacity);
void tcp_close(struct net *net);
void tcp_abort(struct net *net);
int tcp_is_established(const struct net *net);
/* True when the peer has finished and all received data has been read. */
int tcp_is_finished(const struct net *net);
void tcp_receive(struct net *net, uint32_t src, const uint8_t *data, int length);
void tcp_poll(struct net *net);

#endif
