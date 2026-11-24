/* DHCP client (RFC 2131 and RFC 2132). */
#include "net.h"

#define CLIENT_PORT 68
#define SERVER_PORT 67
#define MAGIC 0x63825363u
#define HEADER_SIZE 240
#define PACKET_SIZE 300
#define RETRY_MS 2000u
#define MAX_RETRIES 5

#define OPT_MASK 1
#define OPT_ROUTER 3
#define OPT_DNS 6
#define OPT_REQUESTED 50
#define OPT_MESSAGE 53
#define OPT_SERVER 54
#define OPT_PARAMETERS 55
#define OPT_END 255

#define MSG_DISCOVER 1
#define MSG_OFFER 2
#define MSG_REQUEST 3
#define MSG_ACK 5
#define MSG_NAK 6

static void send_message(struct net *net, int type)
{
    struct dhcp_client *dhcp = &net->dhcp;
    uint8_t packet[PACKET_SIZE];
    __builtin_memset(packet, 0, sizeof packet);

    packet[0] = 1; /* BOOTREQUEST */
    packet[1] = 1; /* Ethernet */
    packet[2] = 6;
    net_put32(packet + 4, dhcp->xid);
    net_put16(packet + 10, 0x8000); /* Ask for a broadcast reply. */
    __builtin_memcpy(packet + 28, net->mac, 6);
    net_put32(packet + 236, MAGIC);

    int o = HEADER_SIZE;
    packet[o++] = OPT_MESSAGE;
    packet[o++] = 1;
    packet[o++] = (uint8_t)type;
    if (type == MSG_REQUEST) {
        packet[o++] = OPT_REQUESTED;
        packet[o++] = 4;
        net_put32(packet + o, dhcp->offered);
        o += 4;
        packet[o++] = OPT_SERVER;
        packet[o++] = 4;
        net_put32(packet + o, dhcp->server);
        o += 4;
    }
    packet[o++] = OPT_PARAMETERS;
    packet[o++] = 3;
    packet[o++] = OPT_MASK;
    packet[o++] = OPT_ROUTER;
    packet[o++] = OPT_DNS;
    packet[o++] = OPT_END;

    net_udp_send(net, 0xffffffffu, CLIENT_PORT, SERVER_PORT, packet, PACKET_SIZE);
    dhcp->deadline = net->now + RETRY_MS;
}

void dhcp_start(struct net *net)
{
    struct dhcp_client *dhcp = &net->dhcp;
    dhcp->phase = DHCP_DISCOVER;
    dhcp->retries = 0;
    dhcp->xid = net->driver.random(net->driver.context);
    send_message(net, MSG_DISCOVER);
}

void dhcp_receive(struct net *net, const uint8_t *p, int length)
{
    struct dhcp_client *dhcp = &net->dhcp;
    if ((dhcp->phase != DHCP_DISCOVER && dhcp->phase != DHCP_REQUEST) || length < HEADER_SIZE)
        return;
    if (p[0] != 2 || net_get32(p + 4) != dhcp->xid || net_get32(p + 236) != MAGIC ||
        __builtin_memcmp(p + 28, net->mac, 6) != 0)
        return;

    int message = 0;
    uint32_t mask = 0, router = 0, dns = 0, server = 0;
    for (int i = HEADER_SIZE; i < length;) {
        int code = p[i++];
        if (code == OPT_END)
            break;
        if (code == 0)
            continue;
        if (i >= length)
            return;
        int size = p[i++];
        if (size > length - i)
            return;
        if (code == OPT_MESSAGE && size >= 1)
            message = p[i];
        else if (code == OPT_MASK && size >= 4)
            mask = net_get32(p + i);
        else if (code == OPT_ROUTER && size >= 4)
            router = net_get32(p + i);
        else if (code == OPT_DNS && size >= 4)
            dns = net_get32(p + i);
        else if (code == OPT_SERVER && size >= 4)
            server = net_get32(p + i);
        i += size;
    }

    uint32_t offered = net_get32(p + 16);
    if (dhcp->phase == DHCP_DISCOVER && message == MSG_OFFER) {
        if (offered == 0 || server == 0)
            return;
        dhcp->offered = offered;
        dhcp->server = server;
        dhcp->phase = DHCP_REQUEST;
        dhcp->retries = 0;
        send_message(net, MSG_REQUEST);
    } else if (dhcp->phase == DHCP_REQUEST && server == dhcp->server) {
        if (message == MSG_NAK) {
            dhcp_start(net);
        } else if (message == MSG_ACK && offered == dhcp->offered && mask != 0) {
            net->ip = offered;
            net->mask = mask;
            net->router = router;
            net->dns_server = dns;
            dhcp->phase = DHCP_BOUND;
            net->state = NET_READY;
        }
    }
}

void dhcp_poll(struct net *net)
{
    struct dhcp_client *dhcp = &net->dhcp;
    if ((dhcp->phase != DHCP_DISCOVER && dhcp->phase != DHCP_REQUEST) ||
        !net_time_reached(net->now, dhcp->deadline))
        return;

    if (++dhcp->retries > MAX_RETRIES) {
        dhcp->phase = DHCP_IDLE;
        net->state = NET_FAILED;
        net->failure = NET_FAIL_NO_DHCP;
        return;
    }
    send_message(net, dhcp->phase == DHCP_DISCOVER ? MSG_DISCOVER : MSG_REQUEST);
}
