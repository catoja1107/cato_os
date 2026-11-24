/* Pure decoder for PS/2 mouse packets: 3 bytes, or 4 with an IntelliMouse wheel. */
#ifndef CATO_BOOT_PS2_PACKET_H
#define CATO_BOOT_PS2_PACKET_H

#include <stdint.h>

#define PS2_PACKET_SYNC 0x08

struct ps2_motion {
    int dx;
    int dy; /* Positive is down the screen. */
    int wheel; /* Positive scrolls down. */
    int left;
    int right;
};

/* Returns 0 if the first byte is not a valid packet start. */
static inline int ps2_packet_decode(const uint8_t *packet, int size, struct ps2_motion *out)
{
    uint8_t flags = packet[0];
    if (!(flags & PS2_PACKET_SYNC))
        return 0;

    out->left = flags & 0x01;
    out->right = (flags >> 1) & 0x01;
    out->wheel = size == 4 ? (int)(int8_t)packet[3] : 0;
    if (out->wheel > 7 || out->wheel < -8)
        out->wheel = 0; /* An IntelliMouse wheel moves at most a few notches per packet. */
    if (flags & 0xc0) {
        /* The X or Y counter overflowed, so the movement is meaningless. */
        out->dx = 0;
        out->dy = 0;
        return 1;
    }
    out->dx = (int)packet[1] - ((flags & 0x10) ? 256 : 0);
    out->dy = -((int)packet[2] - ((flags & 0x20) ? 256 : 0));
    return 1;
}

#endif
