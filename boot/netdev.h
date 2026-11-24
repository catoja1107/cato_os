/* Network adapter access through the firmware's Simple Network Protocol. */
#ifndef CATO_BOOT_NETDEV_H
#define CATO_BOOT_NETDEV_H

#include "efi.h"
#include "net.h"

/* Records firmware handles. The adapter is not touched until the first network use. */
void netdev_init(struct net_driver *driver, EFI_SYSTEM_TABLE *st, EFI_HANDLE image);
/* Stops the adapter and hands it back to the firmware. */
void netdev_shutdown(void);

#endif
