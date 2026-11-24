#ifndef CATO_BOOT_DESKTOP_H
#define CATO_BOOT_DESKTOP_H

#include "efi.h"
#include "gop.h"

/*
 * Runs Cato Desktop until the user exits. The only storage it writes is the
 * approved USB drive (see storage.h). It writes no NVRAM variables.
 */
EFI_STATUS desktop_run(EFI_SYSTEM_TABLE *st, EFI_HANDLE image, const struct framebuffer *fb);

#endif
