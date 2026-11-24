#include "ps2_mouse.h"

#include "cpu.h"

#define I8042_DATA 0x60
#define I8042_STATUS 0x64
#define I8042_COMMAND 0x64

#define STATUS_OUTPUT_FULL 0x01
#define STATUS_INPUT_FULL 0x02
#define STATUS_AUX_DATA 0x20

#define CMD_DISABLE_AUX 0xa7
#define CMD_ENABLE_AUX 0xa8
#define CMD_WRITE_AUX 0xd4

#define MOUSE_GET_ID 0xf2
#define MOUSE_SET_RATE 0xf3
#define MOUSE_SET_DEFAULTS 0xf6
#define MOUSE_ENABLE_REPORTING 0xf4
#define MOUSE_DISABLE_REPORTING 0xf5
#define MOUSE_ACK 0xfa
#define MOUSE_ID_WHEEL 3

#define WAIT_STEPS 1000
#define WAIT_STEP_US 100

static uint8_t g_packet[4];
static int g_packet_len;
static int g_packet_size;
static int g_left;
static int g_right;

static int wait_input_empty(EFI_BOOT_SERVICES *bs)
{
    for (int i = 0; i < WAIT_STEPS; i++) {
        if (!(port_in8(I8042_STATUS) & STATUS_INPUT_FULL))
            return 1;
        bs->Stall(WAIT_STEP_US);
    }
    return 0;
}

static int controller_command(EFI_BOOT_SERVICES *bs, uint8_t command)
{
    if (!wait_input_empty(bs))
        return 0;
    port_out8(I8042_COMMAND, command);
    return 1;
}

/*
 * Only bytes flagged as AUX are read here. Keyboard bytes stay in the controller
 * for the firmware's keyboard driver, which runs from a timer during the Stall.
 */
static int read_mouse_byte(EFI_BOOT_SERVICES *bs, uint8_t *value)
{
    for (int i = 0; i < WAIT_STEPS; i++) {
        uint8_t status = port_in8(I8042_STATUS);
        if ((status & (STATUS_OUTPUT_FULL | STATUS_AUX_DATA)) ==
            (STATUS_OUTPUT_FULL | STATUS_AUX_DATA)) {
            *value = port_in8(I8042_DATA);
            return 1;
        }
        bs->Stall(WAIT_STEP_US);
    }
    return 0;
}

static int mouse_command(EFI_BOOT_SERVICES *bs, uint8_t command)
{
    uint8_t reply;
    if (!controller_command(bs, CMD_WRITE_AUX) || !wait_input_empty(bs))
        return 0;
    port_out8(I8042_DATA, command);
    return read_mouse_byte(bs, &reply) && reply == MOUSE_ACK;
}

/* The IntelliMouse sequence: sample rates 200, 100, 80 switch on the wheel. */
static int enable_wheel(EFI_BOOT_SERVICES *bs)
{
    static const uint8_t RATES[3] = { 200, 100, 80 };
    uint8_t id;

    for (int i = 0; i < 3; i++) {
        if (!mouse_command(bs, MOUSE_SET_RATE) || !mouse_command(bs, RATES[i]))
            return 0;
    }
    return mouse_command(bs, MOUSE_GET_ID) && read_mouse_byte(bs, &id) && id == MOUSE_ID_WHEEL;
}

int ps2_mouse_init(EFI_BOOT_SERVICES *bs)
{
    g_packet_len = 0;
    g_packet_size = 3;
    g_left = 0;
    g_right = 0;

    if (!controller_command(bs, CMD_ENABLE_AUX))
        return 0;
    if (!mouse_command(bs, MOUSE_SET_DEFAULTS)) {
        controller_command(bs, CMD_DISABLE_AUX);
        return 0;
    }
    if (enable_wheel(bs))
        g_packet_size = 4;
    if (!mouse_command(bs, MOUSE_ENABLE_REPORTING)) {
        controller_command(bs, CMD_DISABLE_AUX);
        return 0;
    }
    return 1;
}

int ps2_mouse_poll(struct ps2_motion *motion)
{
    int received = 0;
    motion->dx = 0;
    motion->dy = 0;
    motion->wheel = 0;
    motion->left = g_left;
    motion->right = g_right;

    /* The limit keeps a noisy device from stalling the frame. */
    for (int i = 0; i < 128; i++) {
        uint8_t status = port_in8(I8042_STATUS);
        if ((status & (STATUS_OUTPUT_FULL | STATUS_AUX_DATA)) !=
            (STATUS_OUTPUT_FULL | STATUS_AUX_DATA))
            break;

        uint8_t byte = port_in8(I8042_DATA);
        if (g_packet_len == 0 && !(byte & PS2_PACKET_SYNC))
            continue; /* Resynchronize on the next valid first byte. */

        g_packet[g_packet_len++] = byte;
        if (g_packet_len < g_packet_size)
            continue;
        g_packet_len = 0;

        struct ps2_motion decoded;
        if (ps2_packet_decode(g_packet, g_packet_size, &decoded)) {
            motion->dx += decoded.dx;
            motion->dy += decoded.dy;
            motion->wheel += decoded.wheel;
            motion->left = decoded.left;
            motion->right = decoded.right;
            g_left = decoded.left;
            g_right = decoded.right;
            received = 1;
        }
    }
    return received;
}

void ps2_mouse_shutdown(EFI_BOOT_SERVICES *bs)
{
    mouse_command(bs, MOUSE_DISABLE_REPORTING);
    controller_command(bs, CMD_DISABLE_AUX);
}
