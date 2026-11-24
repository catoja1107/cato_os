/* Ethernet, ARP, IPv4, ICMP echo, and UDP. */
#include "net.h"

#include "text.h"

#define ETH_HEADER 14
#define ETH_MIN_FRAME 60
#define ETH_IPV4 0x0800
#define ETH_ARP 0x0806
#define IP_HEADER 20
#define IP_BROADCAST 0xffffffffu
#define PROTO_ICMP 1
#define PROTO_TCP 6
#define PROTO_UDP 17
#define ARP_PACKET 28
#define ARP_REQUEST 1
#define ARP_REPLY 2
#define ARP_TTL_MS 60000u
#define RX_BUDGET 96
#define PING_TIMEOUT_MS 3000u
#define PING_PAYLOAD 32
#define DHCP_CLIENT_PORT 68

static const uint8_t BROADCAST_MAC[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

uint32_t net_pseudo_sum(uint32_t src, uint32_t dst, uint8_t protocol, int length)
{
    return (src >> 16) + (src & 0xffff) + (dst >> 16) + (dst & 0xffff) + protocol +
           (uint32_t)length;
}

uint16_t net_checksum(const uint8_t *data, int length, uint32_t seed)
{
    uint32_t sum = seed;
    int i = 0;
    for (; i + 1 < length; i += 2)
        sum += net_get16(data + i);
    if (i < length)
        sum += (uint32_t)data[i] << 8;
    while (sum >> 16)
        sum = (sum & 0xffff) + (sum >> 16);
    return (uint16_t)~sum;
}

void net_format_ip(uint32_t ip, char *out, int capacity)
{
    out[0] = 0;
    for (int shift = 24; shift >= 0; shift -= 8) {
        text_append_u64(out, capacity, (ip >> shift) & 0xff);
        if (shift)
            text_append(out, capacity, ".");
    }
}

int net_parse_ip(const char *text, uint32_t *ip)
{
    uint32_t value = 0;
    for (int part = 0; part < 4; part++) {
        int digits = 0;
        uint32_t octet = 0;
        while (*text >= '0' && *text <= '9') {
            octet = octet * 10 + (uint32_t)(*text++ - '0');
            if (++digits > 3 || octet > 255)
                return 0;
        }
        if (digits == 0)
            return 0;
        value = (value << 8) | octet;
        if (part < 3 && *text++ != '.')
            return 0;
    }
    if (*text)
        return 0;
    *ip = value;
    return 1;
}

const char *net_state_text(const struct net *net)
{
    switch (net->state) {
    case NET_OFF:
        return "Not started";
    case NET_CONFIGURING:
        return "Getting an address";
    case NET_READY:
        return "Connected";
    default:
        return net->failure == NET_FAIL_NO_ADAPTER ? "No network adapter" : "No DHCP server";
    }
}

void net_init(struct net *net, const struct net_driver *driver)
{
    __builtin_memset(net, 0, sizeof *net);
    net->driver = *driver;
    net->state = NET_OFF;
    net->tcp.state = TCP_CLOSED;
}

void net_start(struct net *net)
{
    if (net->state == NET_CONFIGURING || net->state == NET_READY)
        return;
    if (!net->driver.start(net->driver.context, net->mac)) {
        net->state = NET_FAILED;
        net->failure = NET_FAIL_NO_ADAPTER;
        return;
    }
    net->state = NET_CONFIGURING;
    net->failure = NET_FAIL_NONE;
    net->ip_id = (uint16_t)net->driver.random(net->driver.context);
    dhcp_start(net);
}

static void send_frame(struct net *net, const uint8_t *dst_mac, uint16_t type, int payload)
{
    uint8_t *frame = net->tx;
    __builtin_memcpy(frame, dst_mac, 6);
    __builtin_memcpy(frame + 6, net->mac, 6);
    net_put16(frame + 12, type);

    int length = ETH_HEADER + payload;
    while (length < ETH_MIN_FRAME)
        frame[length++] = 0;
    net->driver.send(net->driver.context, frame, length);
    net->tx_frames++;
}

static void arp_send(struct net *net, uint16_t operation, const uint8_t *target_mac,
                     uint32_t target_ip)
{
    uint8_t *arp = net->tx + ETH_HEADER;
    net_put16(arp, 1);
    net_put16(arp + 2, ETH_IPV4);
    arp[4] = 6;
    arp[5] = 4;
    net_put16(arp + 6, operation);
    __builtin_memcpy(arp + 8, net->mac, 6);
    net_put32(arp + 14, net->ip);
    if (operation == ARP_REPLY)
        __builtin_memcpy(arp + 18, target_mac, 6);
    else
        __builtin_memset(arp + 18, 0, 6);
    net_put32(arp + 24, target_ip);
    send_frame(net, operation == ARP_REPLY ? target_mac : BROADCAST_MAC, ETH_ARP, ARP_PACKET);
}

static const uint8_t *arp_lookup(const struct net *net, uint32_t ip)
{
    for (int i = 0; i < NET_ARP_ENTRIES; i++) {
        const struct arp_entry *entry = &net->arp[i];
        if (entry->valid && entry->ip == ip && !net_time_reached(net->now, entry->expires))
            return entry->mac;
    }
    return 0;
}

static void arp_store(struct net *net, uint32_t ip, const uint8_t *mac)
{
    struct arp_entry *slot = 0;
    for (int i = 0; i < NET_ARP_ENTRIES && !slot; i++) {
        if (net->arp[i].valid && net->arp[i].ip == ip)
            slot = &net->arp[i];
    }
    for (int i = 0; i < NET_ARP_ENTRIES && !slot; i++) {
        if (!net->arp[i].valid || net_time_reached(net->now, net->arp[i].expires))
            slot = &net->arp[i];
    }
    if (!slot) {
        slot = &net->arp[0];
        for (int i = 1; i < NET_ARP_ENTRIES; i++) {
            if ((int32_t)(net->arp[i].expires - slot->expires) < 0)
                slot = &net->arp[i];
        }
    }
    slot->ip = ip;
    slot->expires = net->now + ARP_TTL_MS;
    slot->valid = 1;
    __builtin_memcpy(slot->mac, mac, 6);
}

static void handle_arp(struct net *net, const uint8_t *p, int length)
{
    if (length < ARP_PACKET || net->ip == 0)
        return;
    if (net_get16(p) != 1 || net_get16(p + 2) != ETH_IPV4 || p[4] != 6 || p[5] != 4)
        return;

    uint16_t operation = net_get16(p + 6);
    uint32_t sender_ip = net_get32(p + 14);
    uint32_t target_ip = net_get32(p + 24);
    if (target_ip != net->ip || sender_ip == 0 || (operation != ARP_REQUEST && operation != ARP_REPLY))
        return;

    uint8_t sender_mac[6];
    __builtin_memcpy(sender_mac, p + 8, 6);
    arp_store(net, sender_ip, sender_mac);

    if (operation == ARP_REQUEST)
        arp_send(net, ARP_REPLY, sender_mac, sender_ip);

    if (net->pending_len > 0 && net->pending_hop == sender_ip) {
        int pending = net->pending_len;
        net->pending_len = 0;
        __builtin_memcpy(net->tx + ETH_HEADER, net->pending, (unsigned)pending);
        send_frame(net, sender_mac, ETH_IPV4, pending);
    }
}

static uint32_t next_hop(const struct net *net, uint32_t dst)
{
    if (dst == IP_BROADCAST || (dst & net->mask) == (net->ip & net->mask))
        return dst;
    return net->router ? net->router : dst;
}

int net_ip_send(struct net *net, uint32_t dst, uint8_t protocol, const uint8_t *data, int length)
{
    if (length < 0 || length + IP_HEADER > NET_MTU)
        return 0;

    uint8_t *ip = net->tx + ETH_HEADER;
    ip[0] = 0x45;
    ip[1] = 0;
    net_put16(ip + 2, (uint16_t)(IP_HEADER + length));
    net_put16(ip + 4, net->ip_id++);
    net_put16(ip + 6, 0x4000); /* Don't Fragment */
    ip[8] = 64;
    ip[9] = protocol;
    net_put16(ip + 10, 0);
    net_put32(ip + 12, net->ip);
    net_put32(ip + 16, dst);
    net_put16(ip + 10, net_checksum(ip, IP_HEADER, 0));
    __builtin_memcpy(ip + IP_HEADER, data, (unsigned)length);

    if (dst == IP_BROADCAST) {
        send_frame(net, BROADCAST_MAC, ETH_IPV4, IP_HEADER + length);
        return 1;
    }

    uint32_t hop = next_hop(net, dst);
    const uint8_t *mac = arp_lookup(net, hop);
    if (mac) {
        uint8_t copy[6];
        __builtin_memcpy(copy, mac, 6);
        send_frame(net, copy, ETH_IPV4, IP_HEADER + length);
        return 1;
    }

    /* Hold one packet until ARP answers. Protocol timers recover anything dropped. */
    __builtin_memcpy(net->pending, ip, (unsigned)(IP_HEADER + length));
    net->pending_len = IP_HEADER + length;
    net->pending_hop = hop;
    arp_send(net, ARP_REQUEST, BROADCAST_MAC, hop);
    return 1;
}

int net_udp_send(struct net *net, uint32_t dst, uint16_t src_port, uint16_t dst_port,
                 const uint8_t *data, int length)
{
    if (length < 0 || length + 8 + IP_HEADER > NET_MTU)
        return 0;

    uint8_t *udp = net->scratch;
    net_put16(udp, src_port);
    net_put16(udp + 2, dst_port);
    net_put16(udp + 4, (uint16_t)(8 + length));
    net_put16(udp + 6, 0);
    __builtin_memcpy(udp + 8, data, (unsigned)length);

    uint16_t sum = net_checksum(udp, 8 + length,
                                net_pseudo_sum(net->ip, dst, PROTO_UDP, 8 + length));
    net_put16(udp + 6, sum ? sum : 0xffff);
    return net_ip_send(net, dst, PROTO_UDP, udp, 8 + length);
}

int net_ping(struct net *net, uint32_t target)
{
    if (net->state != NET_READY)
        return 0;

    struct ping_client *ping = &net->ping;
    ping->state = PING_WAITING;
    ping->id = (uint16_t)net->driver.random(net->driver.context);
    ping->sequence++;
    ping->target = target;
    ping->sent_at = net->now;
    ping->deadline = net->now + PING_TIMEOUT_MS;

    uint8_t *icmp = net->scratch;
    icmp[0] = 8;
    icmp[1] = 0;
    net_put16(icmp + 2, 0);
    net_put16(icmp + 4, ping->id);
    net_put16(icmp + 6, ping->sequence);
    for (int i = 0; i < PING_PAYLOAD; i++)
        icmp[8 + i] = (uint8_t)('a' + i % 26);
    net_put16(icmp + 2, net_checksum(icmp, 8 + PING_PAYLOAD, 0));
    return net_ip_send(net, target, PROTO_ICMP, icmp, 8 + PING_PAYLOAD);
}

/* Only echo replies are handled. The machine does not answer pings from others. */
static void handle_icmp(struct net *net, uint32_t src, const uint8_t *p, int length)
{
    struct ping_client *ping = &net->ping;
    if (length < 8 || net_checksum(p, length, 0) != 0)
        return;
    if (ping->state == PING_WAITING && p[0] == 0 && src == ping->target &&
        net_get16(p + 4) == ping->id && net_get16(p + 6) == ping->sequence) {
        ping->state = PING_DONE;
        ping->round_trip = net->now - ping->sent_at;
    }
}

static void handle_udp(struct net *net, uint32_t src, uint32_t dst, const uint8_t *p, int length)
{
    if (length < 8)
        return;
    int udp_length = net_get16(p + 4);
    if (udp_length < 8 || udp_length > length)
        return;
    if (net_get16(p + 6) != 0 &&
        net_checksum(p, udp_length, net_pseudo_sum(src, dst, PROTO_UDP, udp_length)) != 0)
        return;

    uint16_t src_port = net_get16(p);
    uint16_t dst_port = net_get16(p + 2);
    if (dst_port == DHCP_CLIENT_PORT)
        dhcp_receive(net, p + 8, udp_length - 8);
    else
        dns_receive(net, src, src_port, dst_port, p + 8, udp_length - 8);
}

static void handle_ipv4(struct net *net, const uint8_t *p, int length)
{
    if (length < IP_HEADER || (p[0] >> 4) != 4)
        return;
    int header = (p[0] & 15) * 4;
    if (header < IP_HEADER || header > length)
        return;
    int total = net_get16(p + 2);
    if (total < header || total > length)
        return;
    if (net_checksum(p, header, 0) != 0)
        return;
    /* Deliberate shortcut: fragmented packets are dropped. */
    if (net_get16(p + 6) & 0x3fff)
        return;

    uint32_t src = net_get32(p + 12);
    uint32_t dst = net_get32(p + 16);
    if (net->ip != 0) {
        uint32_t subnet_broadcast = net->ip | ~net->mask;
        if (dst != net->ip && dst != IP_BROADCAST && dst != subnet_broadcast)
            return;
    }

    const uint8_t *payload = p + header;
    int payload_length = total - header;
    switch (p[9]) {
    case PROTO_ICMP:
        if (dst == net->ip)
            handle_icmp(net, src, payload, payload_length);
        break;
    case PROTO_TCP:
        if (dst == net->ip)
            tcp_receive(net, src, payload, payload_length);
        break;
    case PROTO_UDP:
        handle_udp(net, src, dst, payload, payload_length);
        break;
    default:
        break;
    }
}

static void handle_frame(struct net *net, const uint8_t *frame, int length)
{
    if (length < ETH_HEADER)
        return;
    if (__builtin_memcmp(frame, net->mac, 6) != 0 &&
        __builtin_memcmp(frame, BROADCAST_MAC, 6) != 0)
        return;

    uint16_t type = net_get16(frame + 12);
    if (type == ETH_ARP)
        handle_arp(net, frame + ETH_HEADER, length - ETH_HEADER);
    else if (type == ETH_IPV4)
        handle_ipv4(net, frame + ETH_HEADER, length - ETH_HEADER);
}

void net_poll(struct net *net, uint32_t now_ms)
{
    net->now = now_ms;
    if (net->state != NET_CONFIGURING && net->state != NET_READY)
        return;

    for (int i = 0; i < RX_BUDGET; i++) {
        int length = net->driver.receive(net->driver.context, net->rx, NET_FRAME_MAX);
        if (length <= 0)
            break;
        net->rx_frames++;
        handle_frame(net, net->rx, length > NET_FRAME_MAX ? NET_FRAME_MAX : length);
    }

    dhcp_poll(net);
    dns_poll(net);
    tcp_poll(net);
    if (net->ping.state == PING_WAITING && net_time_reached(net->now, net->ping.deadline))
        net->ping.state = PING_FAILED;
}
