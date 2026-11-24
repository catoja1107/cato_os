#include "netdev.h"

#include "cpu.h"

#define TX_SLOTS 16
#define TX_SLOT_SIZE 1536
#define PCI_CLASS_REGISTER 0x08
#define PCI_CLASS_NETWORK 0x02
#define IF_TYPE_ETHERNET 1

static EFI_BOOT_SERVICES *g_bs;
static EFI_HANDLE g_image;
static EFI_HANDLE g_handle;
static EFI_SIMPLE_NETWORK_PROTOCOL *g_snp;
static uint8_t g_tx[TX_SLOTS][TX_SLOT_SIZE];
static uint8_t g_tx_busy[TX_SLOTS];

/* Firmware often leaves network drivers unstarted, so start them for PCI NICs only. */
static void connect_network_controllers(void)
{
    EFI_GUID pci_guid = EFI_PCI_IO_PROTOCOL_GUID;
    UINTN count = 0;
    EFI_HANDLE *handles = NULL;

    if (EFI_ERROR(g_bs->LocateHandleBuffer(ByProtocol, &pci_guid, NULL, &count, &handles)))
        return;
    for (UINTN i = 0; i < count; i++) {
        EFI_PCI_IO_PROTOCOL *pci = NULL;
        uint32_t class_register = 0;
        if (EFI_ERROR(g_bs->HandleProtocol(handles[i], &pci_guid, (void **)&pci)))
            continue;
        if (!EFI_ERROR(pci->PciRead(pci, EfiPciIoWidthUint32, PCI_CLASS_REGISTER, 1,
                                    &class_register)) &&
            (class_register >> 24) == PCI_CLASS_NETWORK)
            g_bs->ConnectController(handles[i], NULL, NULL, 1);
    }
    g_bs->FreePool(handles);
}

static int bring_up(EFI_SIMPLE_NETWORK_PROTOCOL *snp)
{
    EFI_SIMPLE_NETWORK_MODE *mode = snp->Mode;
    if (!mode || mode->IfType != IF_TYPE_ETHERNET || mode->HwAddressSize != 6 ||
        mode->MediaHeaderSize != 14 || mode->MaxPacketSize < NET_MTU)
        return 0;

    if (mode->State == EfiSimpleNetworkStopped && EFI_ERROR(snp->Start(snp)))
        return 0;
    if (mode->State == EfiSimpleNetworkStarted && EFI_ERROR(snp->Initialize(snp, 0, 0)))
        return 0;
    if (mode->State != EfiSimpleNetworkInitialized)
        return 0;

    uint32_t filters = (EFI_SNP_RECEIVE_UNICAST | EFI_SNP_RECEIVE_BROADCAST) &
                       mode->ReceiveFilterMask;
    return !EFI_ERROR(snp->ReceiveFilters(snp, filters, 0, 0, 0, NULL));
}

static int driver_start(void *context, uint8_t mac[6])
{
    (void)context;
    if (!g_snp) {
        EFI_GUID guid = EFI_SIMPLE_NETWORK_PROTOCOL_GUID;
        UINTN count = 0;
        EFI_HANDLE *handles = NULL;

        connect_network_controllers();
        if (EFI_ERROR(g_bs->LocateHandleBuffer(ByProtocol, &guid, NULL, &count, &handles)))
            return 0;

        for (UINTN i = 0; i < count && !g_snp; i++) {
            EFI_SIMPLE_NETWORK_PROTOCOL *snp = NULL;
            /* Exclusive access stops the firmware's own stack from consuming our packets. */
            if (EFI_ERROR(g_bs->OpenProtocol(handles[i], &guid, (void **)&snp, g_image, NULL,
                                             EFI_OPEN_PROTOCOL_EXCLUSIVE)))
                continue;
            if (bring_up(snp)) {
                g_snp = snp;
                g_handle = handles[i];
            } else {
                g_bs->CloseProtocol(handles[i], &guid, g_image, NULL);
                g_bs->ConnectController(handles[i], NULL, NULL, 1);
            }
        }
        g_bs->FreePool(handles);
        if (!g_snp)
            return 0;
    }

    for (int i = 0; i < 6; i++)
        mac[i] = g_snp->Mode->CurrentAddress.Addr[i];
    return 1;
}

static void reclaim_transmit_buffers(void)
{
    for (int attempt = 0; attempt < TX_SLOTS; attempt++) {
        void *buffer = NULL;
        if (EFI_ERROR(g_snp->GetStatus(g_snp, NULL, &buffer)) || !buffer)
            return;
        for (int i = 0; i < TX_SLOTS; i++) {
            if (buffer == (void *)g_tx[i])
                g_tx_busy[i] = 0;
        }
    }
}

static int driver_send(void *context, const uint8_t *frame, int length)
{
    (void)context;
    if (!g_snp || length <= 0 || length > TX_SLOT_SIZE)
        return 0;

    reclaim_transmit_buffers();
    int slot = -1;
    for (int i = 0; i < TX_SLOTS && slot < 0; i++) {
        if (!g_tx_busy[i])
            slot = i;
    }
    if (slot < 0)
        return 0; /* TCP and the other protocol timers will retry. */

    for (int i = 0; i < length; i++)
        g_tx[slot][i] = frame[i];
    if (EFI_ERROR(g_snp->Transmit(g_snp, 0, (UINTN)length, g_tx[slot], NULL, NULL, NULL)))
        return 0;
    g_tx_busy[slot] = 1;
    return 1;
}

static int driver_receive(void *context, uint8_t *frame, int capacity)
{
    (void)context;
    if (!g_snp)
        return 0;
    UINTN size = (UINTN)capacity;
    if (EFI_ERROR(g_snp->Receive(g_snp, NULL, &size, frame, NULL, NULL, NULL)))
        return 0;
    return (int)size;
}

static uint32_t driver_random(void *context)
{
    (void)context;
    return cpu_random32();
}

static int driver_entropy(void *context, uint8_t *out, int length)
{
    (void)context;
    return cpu_entropy(out, length);
}

void netdev_init(struct net_driver *driver, EFI_SYSTEM_TABLE *st, EFI_HANDLE image)
{
    g_bs = st->BootServices;
    g_image = image;
    g_snp = NULL;
    g_handle = NULL;
    for (int i = 0; i < TX_SLOTS; i++)
        g_tx_busy[i] = 0;

    driver->context = NULL;
    driver->start = driver_start;
    driver->send = driver_send;
    driver->receive = driver_receive;
    driver->random = driver_random;
    driver->entropy = driver_entropy;
}

void netdev_shutdown(void)
{
    if (!g_snp)
        return;

    EFI_GUID guid = EFI_SIMPLE_NETWORK_PROTOCOL_GUID;
    g_snp->Shutdown(g_snp);
    g_snp->Stop(g_snp);
    g_bs->CloseProtocol(g_handle, &guid, g_image, NULL);
    g_bs->ConnectController(g_handle, NULL, NULL, 1);
    g_snp = NULL;
}
