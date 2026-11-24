/*
 * A single TCP client connection (RFC 9293).
 *
 * Deliberate shortcuts: out-of-order segments are dropped and re-requested with a
 * duplicate ACK, there is no congestion control because the client sends only a
 * small request, and TIME_WAIT is replaced by choosing a new random local port.
 */
#include "net.h"

#define PROTO_TCP 6
#define HEADER_SIZE 20
#define FLAG_FIN 0x01
#define FLAG_SYN 0x02
#define FLAG_RST 0x04
#define FLAG_PSH 0x08
#define FLAG_ACK 0x10
#define RTO_MS 1000u
#define RTO_MAX_MS 8000u
#define MAX_RETRIES 6

static int seq_after(uint32_t a, uint32_t b)
{
    return (int32_t)(a - b) > 0;
}

static uint16_t advertised_window(const struct tcp_conn *c)
{
    int free_space = TCP_RX_SIZE - c->rx_len;
    return (uint16_t)(free_space > TCP_WINDOW ? TCP_WINDOW : free_space);
}

static void send_segment(struct net *net, uint32_t seq, int flags, const uint8_t *data, int length)
{
    struct tcp_conn *c = &net->tcp;
    uint8_t *s = net->scratch;
    int header = HEADER_SIZE + ((flags & FLAG_SYN) ? 4 : 0);

    net_put16(s, c->local_port);
    net_put16(s + 2, c->remote_port);
    net_put32(s + 4, seq);
    net_put32(s + 8, (flags & FLAG_ACK) ? c->rcv_nxt : 0);
    s[12] = (uint8_t)(header << 2);
    s[13] = (uint8_t)flags;
    net_put16(s + 14, advertised_window(c));
    net_put16(s + 16, 0);
    net_put16(s + 18, 0);
    if (flags & FLAG_SYN) {
        s[20] = 2; /* Maximum Segment Size option */
        s[21] = 4;
        net_put16(s + 22, TCP_MSS);
    }
    if (length > 0)
        __builtin_memcpy(s + header, data, (unsigned)length);

    int total = header + length;
    uint16_t sum = net_checksum(s, total, net_pseudo_sum(net->ip, c->remote_ip, PROTO_TCP, total));
    net_put16(s + 16, sum);
    net_ip_send(net, c->remote_ip, PROTO_TCP, s, total);
}

static void arm_timer(struct net *net)
{
    net->tcp.deadline = net->now + net->tcp.rto;
}

static void fail(struct net *net, int error)
{
    net->tcp.state = TCP_CLOSED;
    net->tcp.error = error;
}

/* Sends any request bytes that fit in the peer's window. */
static void output(struct net *net)
{
    struct tcp_conn *c = &net->tcp;
    while (c->state == TCP_ESTABLISHED || c->state == TCP_CLOSE_WAIT) {
        int offset = (int)(c->snd_nxt - c->tx_base);
        int unsent = c->tx_len - offset;
        int in_flight = (int)(c->snd_nxt - c->snd_una);
        int usable = (int)c->snd_wnd - in_flight;
        if (unsent <= 0 || usable <= 0)
            break;

        int count = unsent < c->mss ? unsent : c->mss;
        if (count > usable)
            count = usable;
        if (in_flight == 0)
            arm_timer(net);
        send_segment(net, c->snd_nxt, FLAG_ACK | FLAG_PSH, c->tx + offset, count);
        c->snd_nxt += (uint32_t)count;
    }
}

int tcp_connect(struct net *net, uint32_t ip, uint16_t port)
{
    if (net->state != NET_READY)
        return 0;
    tcp_abort(net);

    struct tcp_conn *c = &net->tcp;
    uint32_t random_a = net->driver.random(net->driver.context);
    uint32_t random_b = net->driver.random(net->driver.context);
    c->error = TCP_OK;
    c->fin_received = 0;
    c->fin_sent = 0;
    c->mss = 536;
    c->retries = 0;
    c->tx_len = 0;
    c->rx_head = 0;
    c->rx_len = 0;
    c->remote_ip = ip;
    c->remote_port = port;
    c->local_port = (uint16_t)(49152 + random_b % 16384);
    c->iss = random_a;
    c->tx_base = c->iss + 1;
    c->snd_una = c->iss;
    c->snd_nxt = c->iss + 1;
    c->snd_wnd = 0;
    c->rcv_nxt = 0;
    c->rto = RTO_MS;
    c->state = TCP_SYN_SENT;
    arm_timer(net);
    send_segment(net, c->iss, FLAG_SYN, 0, 0);
    return 1;
}

int tcp_write(struct net *net, const uint8_t *data, int length)
{
    struct tcp_conn *c = &net->tcp;
    if ((c->state != TCP_ESTABLISHED && c->state != TCP_CLOSE_WAIT) || length <= 0)
        return 0;
    int space = TCP_TX_SIZE - c->tx_len;
    int count = length < space ? length : space;
    if (count <= 0)
        return 0;
    __builtin_memcpy(c->tx + c->tx_len, data, (unsigned)count);
    c->tx_len += count;
    output(net);
    return count;
}

int tcp_send(struct net *net, const uint8_t *data, int length)
{
    struct tcp_conn *c = &net->tcp;
    if (c->state != TCP_ESTABLISHED || length < 0 || length > TCP_TX_SIZE - c->tx_len)
        return 0;
    return length == 0 || tcp_write(net, data, length) == length;
}

/* Drops acknowledged bytes from the front of the send buffer. */
static void release_acknowledged(struct tcp_conn *c)
{
    int acknowledged = (int)(c->snd_una - c->tx_base);
    if (acknowledged <= 0)
        return;
    if (acknowledged > c->tx_len)
        acknowledged = c->tx_len; /* The FIN uses a sequence number but no buffer space. */
    __builtin_memmove(c->tx, c->tx + acknowledged, (unsigned)(c->tx_len - acknowledged));
    c->tx_len -= acknowledged;
    c->tx_base += (uint32_t)acknowledged;
}

int tcp_read(struct net *net, uint8_t *out, int capacity)
{
    struct tcp_conn *c = &net->tcp;
    int count = c->rx_len < capacity ? c->rx_len : capacity;
    if (count <= 0)
        return 0;

    int window_before = advertised_window(c);
    for (int i = 0; i < count; i++)
        out[i] = c->rx[(c->rx_head + i) % TCP_RX_SIZE];
    c->rx_head = (c->rx_head + count) % TCP_RX_SIZE;
    c->rx_len -= count;

    /* Tell the peer when a nearly closed window has opened again. */
    if (c->state != TCP_CLOSED && window_before < TCP_MSS && advertised_window(c) >= TCP_MSS)
        send_segment(net, c->snd_nxt, FLAG_ACK, 0, 0);
    return count;
}

void tcp_close(struct net *net)
{
    struct tcp_conn *c = &net->tcp;
    if (c->state != TCP_ESTABLISHED && c->state != TCP_CLOSE_WAIT)
        return;

    c->fin_seq = c->snd_nxt;
    c->fin_sent = 1;
    if (c->snd_una == c->snd_nxt)
        arm_timer(net);
    send_segment(net, c->fin_seq, FLAG_ACK | FLAG_FIN, 0, 0);
    c->snd_nxt++;
    c->state = c->state == TCP_ESTABLISHED ? TCP_FIN_WAIT_1 : TCP_LAST_ACK;
}

void tcp_abort(struct net *net)
{
    struct tcp_conn *c = &net->tcp;
    if (c->state != TCP_CLOSED && c->state != TCP_SYN_SENT)
        send_segment(net, c->snd_nxt, FLAG_RST | FLAG_ACK, 0, 0);
    c->state = TCP_CLOSED;
}

int tcp_is_established(const struct net *net)
{
    return net->tcp.state == TCP_ESTABLISHED || net->tcp.state == TCP_CLOSE_WAIT;
}

int tcp_is_finished(const struct net *net)
{
    const struct tcp_conn *c = &net->tcp;
    return c->rx_len == 0 && (c->fin_received || c->state == TCP_CLOSED);
}

static void parse_mss(struct tcp_conn *c, const uint8_t *options, int length)
{
    for (int i = 0; i < length;) {
        int kind = options[i];
        if (kind == 0)
            return;
        if (kind == 1) {
            i++;
            continue;
        }
        if (i + 1 >= length)
            return;
        int size = options[i + 1];
        if (size < 2 || size > length - i)
            return;
        if (kind == 2 && size == 4) {
            int mss = net_get16(options + i + 2);
            if (mss >= 64)
                c->mss = mss < TCP_MSS ? mss : TCP_MSS;
        }
        i += size;
    }
}

static void handle_syn_sent(struct net *net, const uint8_t *p, int header, uint32_t seq,
                            uint32_t ack, int flags, uint32_t window)
{
    struct tcp_conn *c = &net->tcp;
    if ((flags & FLAG_ACK) && ack != c->iss + 1)
        return;
    if (flags & FLAG_RST) {
        if (flags & FLAG_ACK)
            fail(net, TCP_ERR_REFUSED);
        return;
    }
    if (!(flags & FLAG_SYN) || !(flags & FLAG_ACK))
        return;

    parse_mss(c, p + HEADER_SIZE, header - HEADER_SIZE);
    c->rcv_nxt = seq + 1;
    c->snd_una = ack;
    c->snd_wnd = window;
    c->retries = 0;
    c->rto = RTO_MS;
    c->state = TCP_ESTABLISHED;
    send_segment(net, c->snd_nxt, FLAG_ACK, 0, 0);
    output(net);
}

void tcp_receive(struct net *net, uint32_t src, const uint8_t *p, int length)
{
    struct tcp_conn *c = &net->tcp;
    if (c->state == TCP_CLOSED || length < HEADER_SIZE)
        return;
    int header = (p[12] >> 4) * 4;
    if (header < HEADER_SIZE || header > length)
        return;
    if (src != c->remote_ip || net_get16(p) != c->remote_port ||
        net_get16(p + 2) != c->local_port)
        return;
    if (net_checksum(p, length, net_pseudo_sum(src, net->ip, PROTO_TCP, length)) != 0)
        return;

    uint32_t seq = net_get32(p + 4);
    uint32_t ack = net_get32(p + 8);
    int flags = p[13];
    uint32_t window = net_get16(p + 14);

    if (c->state == TCP_SYN_SENT) {
        handle_syn_sent(net, p, header, seq, ack, flags, window);
        return;
    }

    /* Only an exactly in-sequence reset can tear the connection down. */
    if (flags & FLAG_RST) {
        if (seq == c->rcv_nxt)
            fail(net, TCP_ERR_RESET);
        return;
    }
    if (!(flags & FLAG_ACK))
        return;

    if (seq_after(ack, c->snd_una) && !seq_after(ack, c->snd_nxt)) {
        c->snd_una = ack;
        c->retries = 0;
        c->rto = RTO_MS;
        arm_timer(net);
        release_acknowledged(c);
    }
    c->snd_wnd = window;

    if (c->fin_sent && c->snd_una == c->snd_nxt) {
        if (c->state == TCP_FIN_WAIT_1) {
            c->state = TCP_FIN_WAIT_2;
        } else if (c->state == TCP_LAST_ACK) {
            c->state = TCP_CLOSED;
            return;
        }
    }

    const uint8_t *data = p + header;
    int data_length = length - header;
    int must_ack = data_length > 0 || (flags & FLAG_FIN);
    int32_t behind = (int32_t)(c->rcv_nxt - seq);

    if (behind < 0 || behind > data_length) {
        /* Out of order, or an old retransmission. A duplicate ACK says where we are. */
        if (must_ack)
            send_segment(net, c->snd_nxt, FLAG_ACK, 0, 0);
        output(net);
        return;
    }
    data += behind;
    data_length -= behind;

    int accepted = 0;
    if (c->state == TCP_ESTABLISHED || c->state == TCP_FIN_WAIT_1 || c->state == TCP_FIN_WAIT_2) {
        int free_space = TCP_RX_SIZE - c->rx_len;
        accepted = data_length < free_space ? data_length : free_space;
        int tail = (c->rx_head + c->rx_len) % TCP_RX_SIZE;
        for (int i = 0; i < accepted; i++)
            c->rx[(tail + i) % TCP_RX_SIZE] = data[i];
        c->rx_len += accepted;
        c->rcv_nxt += (uint32_t)accepted;
    }

    int close_after_ack = 0;
    if ((flags & FLAG_FIN) && accepted == data_length && !c->fin_received) {
        c->rcv_nxt++;
        c->fin_received = 1;
        if (c->state == TCP_ESTABLISHED)
            c->state = TCP_CLOSE_WAIT;
        else if (c->state == TCP_FIN_WAIT_1)
            c->state = TCP_LAST_ACK;
        else if (c->state == TCP_FIN_WAIT_2)
            close_after_ack = 1;
    }

    if (must_ack)
        send_segment(net, c->snd_nxt, FLAG_ACK, 0, 0);
    if (close_after_ack)
        c->state = TCP_CLOSED;
    else
        output(net);
}

void tcp_poll(struct net *net)
{
    struct tcp_conn *c = &net->tcp;
    if (c->state == TCP_CLOSED || c->snd_una == c->snd_nxt ||
        !net_time_reached(net->now, c->deadline))
        return;

    if (++c->retries > MAX_RETRIES) {
        fail(net, TCP_ERR_TIMEOUT);
        return;
    }
    c->rto = c->rto * 2 > RTO_MAX_MS ? RTO_MAX_MS : c->rto * 2;
    arm_timer(net);

    if (c->state == TCP_SYN_SENT) {
        send_segment(net, c->iss, FLAG_SYN, 0, 0);
        return;
    }

    int offset = (int)(c->snd_una - c->tx_base);
    int remaining = c->tx_len - offset;
    if (remaining > 0)
        send_segment(net, c->snd_una, FLAG_ACK | FLAG_PSH, c->tx + offset,
                     remaining < c->mss ? remaining : c->mss);
    else if (c->fin_sent)
        send_segment(net, c->fin_seq, FLAG_ACK | FLAG_FIN, 0, 0);
}
