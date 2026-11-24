/*
 * Polled PS/2 mouse for virtual machines only. OVMF has no mouse driver, so this
 * gives QEMU a working pointer. Callers must check cpu_in_vm() first, because
 * real machines may not have an i8042 controller.
 */
#ifndef CATO_BOOT_PS2_MOUSE_H
#define CATO_BOOT_PS2_MOUSE_H

#include "efi.h"
#include "ps2_packet.h"

int ps2_mouse_init(EFI_BOOT_SERVICES *bs);
/* Accumulates all pending packets. Returns 1 if any packet arrived. */
int ps2_mouse_poll(struct ps2_motion *motion);
void ps2_mouse_shutdown(EFI_BOOT_SERVICES *bs);

#endif
