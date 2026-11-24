/*
 * Cato OS UEFI loader, Milestone 1.
 *
 * Shows the boot splash for 3 seconds, then runs Cato Desktop. Any key skips the
 * wait, and pressing D on the splash opens a diagnostics screen first. It writes
 * no NVRAM variables, and the only storage it writes is the approved USB drive
 * (see storage.h). ExitBootServices arrives in Milestone 2.
 */
#include "desktop.h"
#include "efi.h"
#include "fmt.h"
#include "gop.h"
#include "splash.h"

#define SPLASH_100NS 30000000ULL /* 3 seconds */

static EFI_SYSTEM_TABLE *g_st;

static void put(const CHAR16 *text)
{
    g_st->ConOut->OutputString(g_st->ConOut, (CHAR16 *)text);
}

static void color(UINTN foreground)
{
    g_st->ConOut->SetAttribute(g_st->ConOut, EFI_TEXT_ATTR(foreground, EFI_BLACK));
}

static void put_dec(uint64_t value)
{
    CHAR16 buffer[24];
    if (fmt_u64(buffer, 24, value, 10, 1))
        put(buffer);
}

static void put_hex(uint64_t value)
{
    CHAR16 buffer[20];
    if (fmt_u64(buffer, 20, value, 16, 16)) {
        put(u"0x");
        put(buffer);
    }
}

static void label(const CHAR16 *name)
{
    color(EFI_LIGHTCYAN);
    put(name);
    color(EFI_LIGHTGRAY);
}

static const CHAR16 *pixel_format_name(EFI_GRAPHICS_PIXEL_FORMAT format)
{
    switch (format) {
    case PixelRedGreenBlueReserved8BitPerColor:
        return u"RGBX 8:8:8:8";
    case PixelBlueGreenRedReserved8BitPerColor:
        return u"BGRX 8:8:8:8";
    case PixelBitMask:
        return u"bit mask";
    default:
        return u"unsupported";
    }
}

/* UEFI encodes revision 2.70 as (2 << 16) | 70, and 2.3.1 as (2 << 16) | 31. */
static void put_uefi_revision(uint32_t revision)
{
    uint32_t minor = revision & 0xffff;
    put_dec(revision >> 16);
    put(u".");
    put_dec(minor / 10);
    if (minor % 10) {
        put(u".");
        put_dec(minor % 10);
    }
}

/* Returns the key's character, or 0 for a non-character key such as an arrow. */
static CHAR16 wait_for_key(void)
{
    EFI_INPUT_KEY key;
    UINTN index;

    g_st->ConIn->Reset(g_st->ConIn, 0);
    for (;;) {
        if (EFI_ERROR(g_st->BootServices->WaitForEvent(1, &g_st->ConIn->WaitForKey, &index)))
            return 0; /* Input is unavailable, so do not spin forever. */

        /* The event can fire without a usable keystroke, such as for a modifier change. */
        if (!EFI_ERROR(g_st->ConIn->ReadKeyStroke(g_st->ConIn, &key)) &&
            (key.ScanCode != 0 || key.UnicodeChar != 0))
            return key.UnicodeChar;
    }
}

/* Holds the splash for a few seconds. Returns the key's character, or 0 if none. */
static CHAR16 wait_on_splash(void)
{
    EFI_BOOT_SERVICES *bs = g_st->BootServices;
    EFI_EVENT timer = NULL;
    CHAR16 result = 0;

    if (EFI_ERROR(bs->CreateEvent(EVT_TIMER, 0, NULL, NULL, &timer)))
        return 0;
    if (EFI_ERROR(bs->SetTimer(timer, TimerRelative, SPLASH_100NS))) {
        bs->CloseEvent(timer);
        return 0;
    }

    EFI_EVENT events[2] = { g_st->ConIn->WaitForKey, timer };
    g_st->ConIn->Reset(g_st->ConIn, 0);
    for (;;) {
        UINTN index = 1;
        if (EFI_ERROR(bs->WaitForEvent(2, events, &index)) || index == 1)
            break;

        EFI_INPUT_KEY key;
        if (!EFI_ERROR(g_st->ConIn->ReadKeyStroke(g_st->ConIn, &key)) &&
            (key.ScanCode != 0 || key.UnicodeChar != 0)) {
            result = key.UnicodeChar;
            break;
        }
    }
    bs->CloseEvent(timer);
    return result;
}

static void print_firmware_details(EFI_HANDLE image)
{
    color(EFI_WHITE);
    put(u"Cato OS - Milestone 1: UEFI boot\r\n\r\n");

    label(u"Firmware:     ");
    put(g_st->FirmwareVendor);
    put(u" (rev ");
    put_hex(g_st->FirmwareRevision);
    put(u")\r\n");

    label(u"UEFI:         ");
    put_uefi_revision(g_st->Hdr.Revision);
    put(u"\r\n");

    /* The load address is what you need to map GDB addresses back to the binary. */
    EFI_GUID loaded_image_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
    EFI_LOADED_IMAGE_PROTOCOL *loaded = NULL;
    if (!EFI_ERROR(g_st->BootServices->HandleProtocol(image, &loaded_image_guid,
                                                      (void **)&loaded))) {
        label(u"Image base:   ");
        put_hex((uint64_t)(uintptr_t)loaded->ImageBase);
        put(u"\r\n");
    }
}

static void print_display_details(const struct framebuffer *fb)
{
    label(u"Display:      ");
    put_dec(fb->width);
    put(u" x ");
    put_dec(fb->height);
    put(u", ");
    put(pixel_format_name(fb->format));
    put(u"\r\n");

    label(u"Scanline:     ");
    put_dec(fb->pixels_per_scanline);
    put(u" pixels\r\n");

    label(u"Framebuffer:  ");
    put_hex(fb->phys_base);
    put(u", ");
    put_dec(fb->size);
    put(u" bytes\r\n");
}

EFI_STATUS EFIAPI efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *st)
{
    g_st = st;

    /* Firmware resets the machine after 5 minutes unless the watchdog is disabled. */
    st->BootServices->SetWatchdogTimer(0, 0, 0, NULL);

    struct framebuffer fb;
    EFI_STATUS status = gop_open(st, &fb);
    if (EFI_ERROR(status)) {
        st->ConOut->ClearScreen(st->ConOut);
        print_firmware_details(image);
        color(EFI_LIGHTRED);
        put(u"\r\nNo usable GOP framebuffer. Status: ");
        put_hex(status);
        put(u"\r\nPress any key to return to firmware.\r\n");
        wait_for_key();
        return status;
    }

    /* Hide the text cursor so it cannot appear on top of the splash. */
    st->ConOut->EnableCursor(st->ConOut, 0);
    st->ConOut->ClearScreen(st->ConOut);
    splash_draw(&fb);

    /* The desktop opens after the timeout or any key. D opens diagnostics first. */
    CHAR16 choice = wait_on_splash();
    if (choice == u'd' || choice == u'D') {
        /* ClearScreen may only clear the text area, so erase the splash directly. */
        gop_fill_rect(&fb, 0, 0, fb.width, fb.height, 0, 0, 0);
        st->ConOut->ClearScreen(st->ConOut);
        print_firmware_details(image);
        print_display_details(&fb);
        gop_draw_test_pattern(&fb);

        color(EFI_LIGHTGREEN);
        put(u"\r\nCATO_M1_BOOT_OK\r\n");
        color(EFI_YELLOW);
        put(u"Press any key to open the desktop.\r\n");
        color(EFI_LIGHTGRAY);
        wait_for_key();
    }

    status = desktop_run(st, image, &fb);

    gop_fill_rect(&fb, 0, 0, fb.width, fb.height, 0, 0, 0);
    st->ConOut->ClearScreen(st->ConOut);
    st->ConOut->EnableCursor(st->ConOut, 1);
    if (EFI_ERROR(status)) {
        color(EFI_LIGHTRED);
        put(u"The desktop could not start. Status: ");
        put_hex(status);
        put(u"\r\nPress any key to return to firmware.\r\n");
        color(EFI_LIGHTGRAY);
        wait_for_key();
        return status;
    }

    put(u"CATO_M1_EXIT\r\n");
    return EFI_SUCCESS;
}
