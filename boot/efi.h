/*
 * Minimal UEFI definitions for the Cato OS loader (x86_64 only).
 *
 * Layouts follow the UEFI Specification sections "EFI System Table",
 * "Services - Boot Services", "Console Support", "Graphics Output Protocol",
 * and "EFI Loaded Image Protocol". Table slots we do not call are declared as
 * plain pointers so that every later member keeps its specified offset. The
 * static assertions at the end of this file guard those offsets.
 */
#ifndef CATO_BOOT_EFI_H
#define CATO_BOOT_EFI_H

#include <stddef.h>
#include <stdint.h>

/* Clang needs the attribute. MSVC-mode editor parsers already default to this ABI. */
#if defined(__clang__) || defined(__GNUC__)
#define EFIAPI __attribute__((ms_abi))
#else
#define EFIAPI
#endif

typedef uint8_t BOOLEAN;
typedef uint64_t UINTN;
typedef uint16_t CHAR16;
typedef UINTN EFI_STATUS;
typedef void *EFI_HANDLE;
typedef void *EFI_EVENT;
typedef uint64_t EFI_PHYSICAL_ADDRESS;

#define EFI_ERROR_BIT (1ULL << 63)
#define EFI_ERROR(status) (((status) & EFI_ERROR_BIT) != 0)
#define EFI_SUCCESS 0ULL
#define EFI_UNSUPPORTED (EFI_ERROR_BIT | 3ULL)
#define EFI_BUFFER_TOO_SMALL (EFI_ERROR_BIT | 5ULL)
#define EFI_NOT_READY (EFI_ERROR_BIT | 6ULL)
#define EFI_NOT_FOUND (EFI_ERROR_BIT | 14ULL)

typedef struct {
    uint32_t Data1;
    uint16_t Data2;
    uint16_t Data3;
    uint8_t Data4[8];
} EFI_GUID;

#define EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID \
    { 0x9042a9de, 0x23dc, 0x4a38, { 0x96, 0xfb, 0x7a, 0xde, 0xd0, 0x80, 0x51, 0x6a } }
#define EFI_LOADED_IMAGE_PROTOCOL_GUID \
    { 0x5b1b31a1, 0x9562, 0x11d2, { 0x8e, 0x3f, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } }
#define EFI_SIMPLE_POINTER_PROTOCOL_GUID \
    { 0x31878c87, 0x0b75, 0x11d5, { 0x9a, 0x4f, 0x00, 0x90, 0x27, 0x3f, 0xc1, 0x4d } }
#define EFI_SIMPLE_NETWORK_PROTOCOL_GUID \
    { 0xa19832b9, 0xac25, 0x11d3, { 0x9a, 0x2d, 0x00, 0x90, 0x27, 0x3f, 0xc1, 0x4d } }
#define EFI_PCI_IO_PROTOCOL_GUID \
    { 0x4cf5b200, 0x68b8, 0x4ca5, { 0x9e, 0xec, 0xb2, 0x3e, 0x3f, 0x50, 0x02, 0x9a } }
#define EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID \
    { 0x964e5b22, 0x6459, 0x11d2, { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } }
#define EFI_BLOCK_IO_PROTOCOL_GUID \
    { 0x964e5b21, 0x6459, 0x11d2, { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } }
#define EFI_DEVICE_PATH_PROTOCOL_GUID \
    { 0x09576e91, 0x6d3f, 0x11d2, { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } }

typedef struct {
    uint64_t Signature;
    uint32_t Revision;
    uint32_t HeaderSize;
    uint32_t CRC32;
    uint32_t Reserved;
} EFI_TABLE_HEADER;

/* Console input */
typedef struct {
    uint16_t ScanCode;
    CHAR16 UnicodeChar;
} EFI_INPUT_KEY;

typedef struct EFI_SIMPLE_TEXT_INPUT_PROTOCOL {
    EFI_STATUS (EFIAPI *Reset)(struct EFI_SIMPLE_TEXT_INPUT_PROTOCOL *This,
                               BOOLEAN ExtendedVerification);
    EFI_STATUS (EFIAPI *ReadKeyStroke)(struct EFI_SIMPLE_TEXT_INPUT_PROTOCOL *This,
                                       EFI_INPUT_KEY *Key);
    EFI_EVENT WaitForKey;
} EFI_SIMPLE_TEXT_INPUT_PROTOCOL;

/* Console output */
#define EFI_BLACK 0x00
#define EFI_LIGHTGRAY 0x07
#define EFI_LIGHTGREEN 0x0a
#define EFI_LIGHTCYAN 0x0b
#define EFI_LIGHTRED 0x0c
#define EFI_YELLOW 0x0e
#define EFI_WHITE 0x0f
#define EFI_TEXT_ATTR(fg, bg) ((UINTN)(fg) | ((UINTN)(bg) << 4))

typedef struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL {
    void *Reset;
    EFI_STATUS (EFIAPI *OutputString)(struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This,
                                      CHAR16 *String);
    void *TestString;
    void *QueryMode;
    void *SetMode;
    EFI_STATUS (EFIAPI *SetAttribute)(struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This,
                                      UINTN Attribute);
    EFI_STATUS (EFIAPI *ClearScreen)(struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This);
    void *SetCursorPosition;
    EFI_STATUS (EFIAPI *EnableCursor)(struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This,
                                      BOOLEAN Visible);
    void *Mode;
} EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;

/* Graphics Output Protocol */
typedef enum {
    PixelRedGreenBlueReserved8BitPerColor = 0,
    PixelBlueGreenRedReserved8BitPerColor = 1,
    PixelBitMask = 2,
    PixelBltOnly = 3,
    PixelFormatMax = 4
} EFI_GRAPHICS_PIXEL_FORMAT;

typedef struct {
    uint32_t RedMask;
    uint32_t GreenMask;
    uint32_t BlueMask;
    uint32_t ReservedMask;
} EFI_PIXEL_BITMASK;

typedef struct {
    uint32_t Version;
    uint32_t HorizontalResolution;
    uint32_t VerticalResolution;
    EFI_GRAPHICS_PIXEL_FORMAT PixelFormat;
    EFI_PIXEL_BITMASK PixelInformation;
    uint32_t PixelsPerScanLine;
} EFI_GRAPHICS_OUTPUT_MODE_INFORMATION;

typedef struct {
    uint32_t MaxMode;
    uint32_t Mode;
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *Info;
    UINTN SizeOfInfo;
    EFI_PHYSICAL_ADDRESS FrameBufferBase;
    UINTN FrameBufferSize;
} EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE;

typedef struct {
    void *QueryMode;
    void *SetMode;
    void *Blt;
    EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE *Mode;
} EFI_GRAPHICS_OUTPUT_PROTOCOL;

/* Simple Pointer Protocol */
typedef struct {
    int32_t RelativeMovementX;
    int32_t RelativeMovementY;
    int32_t RelativeMovementZ;
    BOOLEAN LeftButton;
    BOOLEAN RightButton;
} EFI_SIMPLE_POINTER_STATE;

typedef struct {
    uint64_t ResolutionX;
    uint64_t ResolutionY;
    uint64_t ResolutionZ;
    BOOLEAN LeftButton;
    BOOLEAN RightButton;
} EFI_SIMPLE_POINTER_MODE;

typedef struct EFI_SIMPLE_POINTER_PROTOCOL {
    EFI_STATUS (EFIAPI *Reset)(struct EFI_SIMPLE_POINTER_PROTOCOL *This,
                               BOOLEAN ExtendedVerification);
    EFI_STATUS (EFIAPI *GetState)(struct EFI_SIMPLE_POINTER_PROTOCOL *This,
                                  EFI_SIMPLE_POINTER_STATE *State);
    EFI_EVENT WaitForInput;
    EFI_SIMPLE_POINTER_MODE *Mode;
} EFI_SIMPLE_POINTER_PROTOCOL;

/* Runtime Services. Only the members before and including GetTime are declared. */
typedef struct {
    uint16_t Year;
    uint8_t Month;
    uint8_t Day;
    uint8_t Hour;
    uint8_t Minute;
    uint8_t Second;
    uint8_t Pad1;
    uint32_t Nanosecond;
    int16_t TimeZone;
    uint8_t Daylight;
    uint8_t Pad2;
} EFI_TIME;

typedef struct {
    EFI_TABLE_HEADER Hdr;
    EFI_STATUS (EFIAPI *GetTime)(EFI_TIME *Time, void *Capabilities);
} EFI_RUNTIME_SERVICES;

/* Memory */
#define EfiLoaderCode 1u
#define EfiLoaderData 2u
#define EfiBootServicesCode 3u
#define EfiBootServicesData 4u
#define EfiConventionalMemory 7u
#define AllocateAnyPages 0u

typedef struct {
    uint32_t Type;
    EFI_PHYSICAL_ADDRESS PhysicalStart;
    uint64_t VirtualStart;
    uint64_t NumberOfPages;
    uint64_t Attribute;
} EFI_MEMORY_DESCRIPTOR;

/* Simple Network Protocol (UEFI Specification, "Network Protocols - SNP, PXE, BIS and HTTP Boot") */
#define EfiSimpleNetworkStopped 0u
#define EfiSimpleNetworkStarted 1u
#define EfiSimpleNetworkInitialized 2u
#define EFI_SNP_RECEIVE_UNICAST 0x01u
#define EFI_SNP_RECEIVE_BROADCAST 0x04u
#define EFI_OPEN_PROTOCOL_EXCLUSIVE 0x20u

typedef struct {
    uint8_t Addr[32];
} EFI_MAC_ADDRESS;

typedef struct {
    uint32_t State;
    uint32_t HwAddressSize;
    uint32_t MediaHeaderSize;
    uint32_t MaxPacketSize;
    uint32_t NvRamSize;
    uint32_t NvRamAccessSize;
    uint32_t ReceiveFilterMask;
    uint32_t ReceiveFilterSetting;
    uint32_t MaxMCastFilterCount;
    uint32_t MCastFilterCount;
    EFI_MAC_ADDRESS MCastFilter[16];
    EFI_MAC_ADDRESS CurrentAddress;
    EFI_MAC_ADDRESS BroadcastAddress;
    EFI_MAC_ADDRESS PermanentAddress;
    uint8_t IfType;
    BOOLEAN MacAddressChangeable;
    BOOLEAN MultipleTxSupported;
    BOOLEAN MediaPresentSupported;
    BOOLEAN MediaPresent;
} EFI_SIMPLE_NETWORK_MODE;

typedef struct EFI_SIMPLE_NETWORK_PROTOCOL {
    uint64_t Revision;
    EFI_STATUS (EFIAPI *Start)(struct EFI_SIMPLE_NETWORK_PROTOCOL *This);
    EFI_STATUS (EFIAPI *Stop)(struct EFI_SIMPLE_NETWORK_PROTOCOL *This);
    EFI_STATUS (EFIAPI *Initialize)(struct EFI_SIMPLE_NETWORK_PROTOCOL *This,
                                    UINTN ExtraRxBufferSize, UINTN ExtraTxBufferSize);
    void *Reset;
    EFI_STATUS (EFIAPI *Shutdown)(struct EFI_SIMPLE_NETWORK_PROTOCOL *This);
    EFI_STATUS (EFIAPI *ReceiveFilters)(struct EFI_SIMPLE_NETWORK_PROTOCOL *This, uint32_t Enable,
                                        uint32_t Disable, BOOLEAN ResetMCastFilter,
                                        UINTN MCastFilterCnt, EFI_MAC_ADDRESS *MCastFilter);
    void *StationAddress;
    void *Statistics;
    void *MCastIpToMac;
    void *NvData;
    EFI_STATUS (EFIAPI *GetStatus)(struct EFI_SIMPLE_NETWORK_PROTOCOL *This,
                                   uint32_t *InterruptStatus, void **TxBuf);
    EFI_STATUS (EFIAPI *Transmit)(struct EFI_SIMPLE_NETWORK_PROTOCOL *This, UINTN HeaderSize,
                                  UINTN BufferSize, void *Buffer, EFI_MAC_ADDRESS *SrcAddr,
                                  EFI_MAC_ADDRESS *DestAddr, uint16_t *Protocol);
    EFI_STATUS (EFIAPI *Receive)(struct EFI_SIMPLE_NETWORK_PROTOCOL *This, UINTN *HeaderSize,
                                 UINTN *BufferSize, void *Buffer, EFI_MAC_ADDRESS *SrcAddr,
                                 EFI_MAC_ADDRESS *DestAddr, uint16_t *Protocol);
    EFI_EVENT WaitForPacket;
    EFI_SIMPLE_NETWORK_MODE *Mode;
} EFI_SIMPLE_NETWORK_PROTOCOL;

/* PCI I/O Protocol: only the configuration-space read is used. */
#define EfiPciIoWidthUint32 2u

typedef struct EFI_PCI_IO_PROTOCOL {
    void *PollMem;
    void *PollIo;
    void *MemRead;
    void *MemWrite;
    void *IoRead;
    void *IoWrite;
    EFI_STATUS (EFIAPI *PciRead)(struct EFI_PCI_IO_PROTOCOL *This, uint32_t Width, uint32_t Offset,
                                 UINTN Count, void *Buffer);
} EFI_PCI_IO_PROTOCOL;

/* Device paths, Block I/O, and files (UEFI Specification, "Media Access") */
#define EFI_DEVICE_PATH_MESSAGING 3
#define EFI_DEVICE_PATH_MSG_USB 5
#define EFI_DEVICE_PATH_END 0x7f
#define EFI_FILE_MODE_READ 0x0000000000000001ULL
#define EFI_FILE_MODE_WRITE 0x0000000000000002ULL
#define EFI_FILE_MODE_CREATE 0x8000000000000000ULL

typedef struct {
    uint8_t Type;
    uint8_t SubType;
    uint8_t Length[2];
} EFI_DEVICE_PATH_PROTOCOL;

typedef struct {
    uint32_t MediaId;
    BOOLEAN RemovableMedia;
    BOOLEAN MediaPresent;
    BOOLEAN LogicalPartition;
    BOOLEAN ReadOnly;
    BOOLEAN WriteCaching;
} EFI_BLOCK_IO_MEDIA;

/* Raw block reads and writes are deliberately not declared, so they cannot be called. */
typedef struct {
    uint64_t Revision;
    EFI_BLOCK_IO_MEDIA *Media;
} EFI_BLOCK_IO_PROTOCOL;

typedef struct EFI_FILE_PROTOCOL {
    uint64_t Revision;
    EFI_STATUS (EFIAPI *Open)(struct EFI_FILE_PROTOCOL *This, struct EFI_FILE_PROTOCOL **NewHandle,
                              CHAR16 *FileName, uint64_t OpenMode, uint64_t Attributes);
    EFI_STATUS (EFIAPI *Close)(struct EFI_FILE_PROTOCOL *This);
    void *Delete;
    EFI_STATUS (EFIAPI *Read)(struct EFI_FILE_PROTOCOL *This, UINTN *BufferSize, void *Buffer);
    EFI_STATUS (EFIAPI *Write)(struct EFI_FILE_PROTOCOL *This, UINTN *BufferSize, void *Buffer);
    void *GetPosition;
    EFI_STATUS (EFIAPI *SetPosition)(struct EFI_FILE_PROTOCOL *This, uint64_t Position);
    void *GetInfo;
    void *SetInfo;
    EFI_STATUS (EFIAPI *Flush)(struct EFI_FILE_PROTOCOL *This);
} EFI_FILE_PROTOCOL;

typedef struct EFI_SIMPLE_FILE_SYSTEM_PROTOCOL {
    uint64_t Revision;
    EFI_STATUS (EFIAPI *OpenVolume)(struct EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *This,
                                    EFI_FILE_PROTOCOL **Root);
} EFI_SIMPLE_FILE_SYSTEM_PROTOCOL;

/* Events */
#define EVT_TIMER 0x80000000u
#define TimerCancel 0u
#define TimerPeriodic 1u
#define TimerRelative 2u

/* Boot Services */
typedef enum {
    AllHandles = 0,
    ByRegisterNotify = 1,
    ByProtocol = 2
} EFI_LOCATE_SEARCH_TYPE;

typedef struct {
    EFI_TABLE_HEADER Hdr;
    void *RaiseTPL;
    void *RestoreTPL;
    EFI_STATUS (EFIAPI *AllocatePages)(uint32_t Type, uint32_t MemoryType, UINTN Pages,
                                       EFI_PHYSICAL_ADDRESS *Memory);
    EFI_STATUS (EFIAPI *FreePages)(EFI_PHYSICAL_ADDRESS Memory, UINTN Pages);
    EFI_STATUS (EFIAPI *GetMemoryMap)(UINTN *MemoryMapSize, void *MemoryMap, UINTN *MapKey,
                                      UINTN *DescriptorSize, uint32_t *DescriptorVersion);
    EFI_STATUS (EFIAPI *AllocatePool)(uint32_t PoolType, UINTN Size, void **Buffer);
    EFI_STATUS (EFIAPI *FreePool)(void *Buffer);
    EFI_STATUS (EFIAPI *CreateEvent)(uint32_t Type, UINTN NotifyTpl, void *NotifyFunction,
                                     void *NotifyContext, EFI_EVENT *Event);
    EFI_STATUS (EFIAPI *SetTimer)(EFI_EVENT Event, uint32_t Type, uint64_t TriggerTime);
    EFI_STATUS (EFIAPI *WaitForEvent)(UINTN NumberOfEvents, EFI_EVENT *Event, UINTN *Index);
    void *SignalEvent;
    EFI_STATUS (EFIAPI *CloseEvent)(EFI_EVENT Event);
    void *CheckEvent;
    void *InstallProtocolInterface;
    void *ReinstallProtocolInterface;
    void *UninstallProtocolInterface;
    EFI_STATUS (EFIAPI *HandleProtocol)(EFI_HANDLE Handle, EFI_GUID *Protocol, void **Interface);
    void *Reserved;
    void *RegisterProtocolNotify;
    void *LocateHandle;
    void *LocateDevicePath;
    void *InstallConfigurationTable;
    void *LoadImage;
    void *StartImage;
    void *Exit;
    void *UnloadImage;
    void *ExitBootServices;
    void *GetNextMonotonicCount;
    EFI_STATUS (EFIAPI *Stall)(UINTN Microseconds);
    EFI_STATUS (EFIAPI *SetWatchdogTimer)(UINTN Timeout, uint64_t WatchdogCode,
                                          UINTN DataSize, CHAR16 *WatchdogData);
    EFI_STATUS (EFIAPI *ConnectController)(EFI_HANDLE ControllerHandle,
                                           EFI_HANDLE *DriverImageHandle,
                                           void *RemainingDevicePath, BOOLEAN Recursive);
    void *DisconnectController;
    EFI_STATUS (EFIAPI *OpenProtocol)(EFI_HANDLE Handle, EFI_GUID *Protocol, void **Interface,
                                      EFI_HANDLE AgentHandle, EFI_HANDLE ControllerHandle,
                                      uint32_t Attributes);
    EFI_STATUS (EFIAPI *CloseProtocol)(EFI_HANDLE Handle, EFI_GUID *Protocol,
                                       EFI_HANDLE AgentHandle, EFI_HANDLE ControllerHandle);
    void *OpenProtocolInformation;
    void *ProtocolsPerHandle;
    EFI_STATUS (EFIAPI *LocateHandleBuffer)(EFI_LOCATE_SEARCH_TYPE SearchType, EFI_GUID *Protocol,
                                            void *SearchKey, UINTN *NoHandles,
                                            EFI_HANDLE **Buffer);
    void *LocateProtocol;
    void *InstallMultipleProtocolInterfaces;
    void *UninstallMultipleProtocolInterfaces;
    void *CalculateCrc32;
    void *CopyMem;
    void *SetMem;
    void *CreateEventEx;
} EFI_BOOT_SERVICES;

/* System Table */
typedef struct {
    EFI_TABLE_HEADER Hdr;
    CHAR16 *FirmwareVendor;
    uint32_t FirmwareRevision;
    EFI_HANDLE ConsoleInHandle;
    EFI_SIMPLE_TEXT_INPUT_PROTOCOL *ConIn;
    EFI_HANDLE ConsoleOutHandle;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *ConOut;
    EFI_HANDLE StandardErrorHandle;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *StdErr;
    EFI_RUNTIME_SERVICES *RuntimeServices;
    EFI_BOOT_SERVICES *BootServices;
    UINTN NumberOfTableEntries;
    void *ConfigurationTable;
} EFI_SYSTEM_TABLE;

/* Loaded Image Protocol */
typedef struct {
    uint32_t Revision;
    EFI_HANDLE ParentHandle;
    EFI_SYSTEM_TABLE *SystemTable;
    EFI_HANDLE DeviceHandle;
    void *FilePath;
    void *Reserved;
    uint32_t LoadOptionsSize;
    void *LoadOptions;
    void *ImageBase;
    uint64_t ImageSize;
    uint32_t ImageCodeType;
    uint32_t ImageDataType;
    void *Unload;
} EFI_LOADED_IMAGE_PROTOCOL;

/* Layout guards for the x86_64 ABI. */
_Static_assert(sizeof(EFI_TABLE_HEADER) == 24, "EFI_TABLE_HEADER size");
_Static_assert(sizeof(EFI_TIME) == 16, "EFI_TIME size");
_Static_assert(offsetof(EFI_RUNTIME_SERVICES, GetTime) == 0x18, "RuntimeServices.GetTime");
_Static_assert(offsetof(EFI_MEMORY_DESCRIPTOR, NumberOfPages) == 24, "MemoryDescriptor.NumberOfPages");
_Static_assert(offsetof(EFI_BOOT_SERVICES, GetMemoryMap) == 0x38, "BootServices.GetMemoryMap");
_Static_assert(offsetof(EFI_BOOT_SERVICES, CreateEvent) == 0x50, "BootServices.CreateEvent");
_Static_assert(offsetof(EFI_BOOT_SERVICES, CloseEvent) == 0x70, "BootServices.CloseEvent");
_Static_assert(offsetof(EFI_BOOT_SERVICES, Stall) == 0xf8, "BootServices.Stall");
_Static_assert(offsetof(EFI_BOOT_SERVICES, ConnectController) == 0x108, "BootServices.ConnectController");
_Static_assert(offsetof(EFI_BOOT_SERVICES, OpenProtocol) == 0x118, "BootServices.OpenProtocol");
_Static_assert(offsetof(EFI_BOOT_SERVICES, CloseProtocol) == 0x120, "BootServices.CloseProtocol");
_Static_assert(offsetof(EFI_SIMPLE_NETWORK_PROTOCOL, GetStatus) == 88, "SNP.GetStatus");
_Static_assert(offsetof(EFI_SIMPLE_NETWORK_PROTOCOL, Receive) == 104, "SNP.Receive");
_Static_assert(offsetof(EFI_SIMPLE_NETWORK_PROTOCOL, Mode) == 120, "SNP.Mode");
_Static_assert(offsetof(EFI_SIMPLE_NETWORK_MODE, CurrentAddress) == 552, "SNP.Mode.CurrentAddress");
_Static_assert(offsetof(EFI_SIMPLE_NETWORK_MODE, MediaPresent) == 652, "SNP.Mode.MediaPresent");
_Static_assert(offsetof(EFI_PCI_IO_PROTOCOL, PciRead) == 48, "PciIo.Pci.Read");
_Static_assert(offsetof(EFI_BLOCK_IO_MEDIA, ReadOnly) == 7, "BlockIoMedia.ReadOnly");
_Static_assert(offsetof(EFI_BLOCK_IO_PROTOCOL, Media) == 8, "BlockIo.Media");
_Static_assert(offsetof(EFI_FILE_PROTOCOL, Read) == 32, "File.Read");
_Static_assert(offsetof(EFI_FILE_PROTOCOL, SetPosition) == 56, "File.SetPosition");
_Static_assert(offsetof(EFI_FILE_PROTOCOL, Flush) == 80, "File.Flush");
_Static_assert(offsetof(EFI_SYSTEM_TABLE, ConOut) == 0x40, "SystemTable.ConOut");
_Static_assert(offsetof(EFI_SYSTEM_TABLE, BootServices) == 0x60, "SystemTable.BootServices");
_Static_assert(offsetof(EFI_BOOT_SERVICES, FreePool) == 0x48, "BootServices.FreePool");
_Static_assert(offsetof(EFI_BOOT_SERVICES, WaitForEvent) == 0x60, "BootServices.WaitForEvent");
_Static_assert(offsetof(EFI_BOOT_SERVICES, HandleProtocol) == 0x98, "BootServices.HandleProtocol");
_Static_assert(offsetof(EFI_BOOT_SERVICES, SetWatchdogTimer) == 0x100, "BootServices.SetWatchdogTimer");
_Static_assert(offsetof(EFI_BOOT_SERVICES, LocateHandleBuffer) == 0x138, "BootServices.LocateHandleBuffer");
_Static_assert(sizeof(EFI_BOOT_SERVICES) == 0x178, "EFI_BOOT_SERVICES size");
_Static_assert(offsetof(EFI_LOADED_IMAGE_PROTOCOL, ImageBase) == 0x40, "LoadedImage.ImageBase");
_Static_assert(offsetof(EFI_GRAPHICS_OUTPUT_MODE_INFORMATION, PixelsPerScanLine) == 32,
               "ModeInformation.PixelsPerScanLine");

#endif
