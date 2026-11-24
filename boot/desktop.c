/* UEFI platform layer for Cato Desktop: memory, input, time, and display output. */
#include "desktop.h"

#include "cpu.h"
#include "netdev.h"
#include "ps2_mouse.h"
#include "shell.h"
#include "splash.h"
#include "storage.h"
#include "text.h"
#include "wm.h"

#define FRAME_100NS 160000 /* 16 ms */
#define FRAME_MS 16
#define SAVE_DELAY_FRAMES 60 /* Wait about a second after the last change. */
#define CLOCK_FRAMES 30
#define MAX_KEYS_PER_FRAME 32
#define DEBUGCON_PORT 0xe9

/* UEFI scan codes (UEFI Specification, "Simple Text Input Protocol"). */
#define SCAN_UP 0x01
#define SCAN_DOWN 0x02
#define SCAN_RIGHT 0x03
#define SCAN_LEFT 0x04
#define SCAN_HOME 0x05
#define SCAN_END 0x06
#define SCAN_DELETE 0x08
#define SCAN_PGUP 0x09
#define SCAN_PGDN 0x0a
#define SCAN_F1 0x0b
#define SCAN_F2 0x0c
#define SCAN_F3 0x0d
#define SCAN_F4 0x0e
#define SCAN_F5 0x0f
#define SCAN_F6 0x10
#define SCAN_F7 0x11
#define SCAN_F8 0x12
#define SCAN_ESC 0x17

static struct wm g_wm;
static struct sysinfo g_info;
static struct fs g_fs;
static struct net g_net;

/* Test events go to QEMU's debug console. This is only installed inside a VM. */
static void debugcon_trace(const char *event)
{
    for (; *event; event++)
        port_out8(DEBUGCON_PORT, (uint8_t)*event);
    port_out8(DEBUGCON_PORT, '\n');
}

static void trace_storage(const char *verb)
{
    char event[64];
    text_copy(event, sizeof event, verb);
    text_append_u64(event, sizeof event, fs_used_bytes(&g_fs));
    text_append(event, sizeof event, ":");
    text_append_u64(event, sizeof event, (uint64_t)g_fs.free_nodes);
    wm_trace(&g_wm, event);
}

static int ui_scale(uint32_t width, uint32_t height)
{
    uint32_t by_width = width / 1280;
    uint32_t by_height = height / 800;
    uint32_t scale = by_width < by_height ? by_width : by_height;
    return scale < 1 ? 1 : (int)scale;
}

static void narrow_text(char *out, int capacity, const CHAR16 *text)
{
    int i = 0;
    for (; text && text[i] && i < capacity - 1; i++)
        out[i] = text[i] < 0x7f && text[i] >= 0x20 ? (char)text[i] : '?';
    out[i] = 0;
}

static void read_cpu_brand(char *out, int capacity)
{
    uint32_t regs[4];
    char brand[49];

    cpu_cpuid(0x80000000u, regs);
    if (regs[0] < 0x80000004u) {
        text_copy(out, capacity, "x86_64");
        return;
    }
    for (uint32_t leaf = 0; leaf < 3; leaf++) {
        cpu_cpuid(0x80000002u + leaf, regs);
        for (uint32_t i = 0; i < 16; i++)
            brand[leaf * 16 + i] = (char)(regs[i / 4] >> ((i % 4) * 8));
    }
    brand[48] = 0;

    const char *start = brand;
    while (*start == ' ')
        start++;
    text_copy(out, capacity, start);
}

/* Uses the descriptor size firmware reports, which can exceed sizeof(descriptor). */
static uint64_t usable_memory_bytes(EFI_BOOT_SERVICES *bs)
{
    UINTN map_size = 0, map_key = 0, descriptor_size = 0;
    uint32_t version = 0;
    void *map = NULL;
    uint64_t total = 0;

    if (bs->GetMemoryMap(&map_size, NULL, &map_key, &descriptor_size, &version) !=
        EFI_BUFFER_TOO_SMALL)
        return 0;

    /* Allocating the buffer can itself add map entries. */
    map_size += descriptor_size * 8;
    if (EFI_ERROR(bs->AllocatePool(EfiLoaderData, map_size, &map)))
        return 0;

    if (!EFI_ERROR(bs->GetMemoryMap(&map_size, map, &map_key, &descriptor_size, &version))) {
        for (UINTN offset = 0; offset + descriptor_size <= map_size; offset += descriptor_size) {
            const EFI_MEMORY_DESCRIPTOR *d =
                (const EFI_MEMORY_DESCRIPTOR *)((const uint8_t *)map + offset);
            if (d->Type == EfiLoaderCode || d->Type == EfiLoaderData ||
                d->Type == EfiBootServicesCode || d->Type == EfiBootServicesData ||
                d->Type == EfiConventionalMemory)
                total += d->NumberOfPages * 4096;
        }
    }
    bs->FreePool(map);
    return total;
}

static void gather_sysinfo(EFI_SYSTEM_TABLE *st, const struct framebuffer *fb, int scale,
                           const char *pointer)
{
    narrow_text(g_info.firmware, sizeof g_info.firmware, st->FirmwareVendor);

    uint32_t minor = st->Hdr.Revision & 0xffff;
    g_info.uefi[0] = 0;
    text_append_u64(g_info.uefi, sizeof g_info.uefi, st->Hdr.Revision >> 16);
    text_append(g_info.uefi, sizeof g_info.uefi, ".");
    text_append_u64(g_info.uefi, sizeof g_info.uefi, minor / 10);
    if (minor % 10) {
        text_append(g_info.uefi, sizeof g_info.uefi, ".");
        text_append_u64(g_info.uefi, sizeof g_info.uefi, minor % 10);
    }

    read_cpu_brand(g_info.cpu, sizeof g_info.cpu);

    uint64_t mib = usable_memory_bytes(st->BootServices) / (1024 * 1024);
    g_info.memory[0] = 0;
    if (mib >= 1024) {
        uint64_t tenths = mib * 10 / 1024;
        text_append_u64(g_info.memory, sizeof g_info.memory, tenths / 10);
        text_append(g_info.memory, sizeof g_info.memory, ".");
        text_append_u64(g_info.memory, sizeof g_info.memory, tenths % 10);
        text_append(g_info.memory, sizeof g_info.memory, " GiB usable");
    } else {
        text_append_u64(g_info.memory, sizeof g_info.memory, mib);
        text_append(g_info.memory, sizeof g_info.memory, " MiB usable");
    }

    g_info.display[0] = 0;
    text_append_u64(g_info.display, sizeof g_info.display, fb->width);
    text_append(g_info.display, sizeof g_info.display, " x ");
    text_append_u64(g_info.display, sizeof g_info.display, fb->height);
    text_append(g_info.display, sizeof g_info.display, " at ");
    text_append_u64(g_info.display, sizeof g_info.display, (uint64_t)scale);
    text_append(g_info.display, sizeof g_info.display, "x UI");

    text_copy(g_info.pointer, sizeof g_info.pointer, pointer);
}

static void present(const struct framebuffer *fb, const uint32_t *back, int width, int scale,
                    struct rect r)
{
    int native = fb->layout.red.shift == 16 && fb->layout.red.bits == 8 &&
                 fb->layout.green.shift == 8 && fb->layout.green.bits == 8 &&
                 fb->layout.blue.shift == 0 && fb->layout.blue.bits == 8;

    for (int y = r.y; y < r.y + r.h; y++) {
        const uint32_t *source = back + (int64_t)y * width + r.x;
        for (int repeat = 0; repeat < scale; repeat++) {
            volatile uint32_t *line =
                fb->base + (uint64_t)(y * scale + repeat) * fb->pixels_per_scanline +
                (uint64_t)r.x * (uint64_t)scale;
            for (int x = 0; x < r.w; x++) {
                uint32_t color = source[x];
                if (!native)
                    color = pixel_pack(&fb->layout, (uint8_t)(color >> 16),
                                       (uint8_t)(color >> 8), (uint8_t)color);
                for (int s = 0; s < scale; s++)
                    *line++ = color;
            }
        }
    }
}

static int translate_key(const EFI_INPUT_KEY *key)
{
    if (key->UnicodeChar == '\r')
        return KEY_ENTER;
    if (key->UnicodeChar == '\b')
        return KEY_BACKSPACE;
    if (key->UnicodeChar == '\t')
        return KEY_TAB;
    /* Firmware reports Ctrl+A through Ctrl+Z as the control codes 1 through 26. */
    if (key->UnicodeChar >= 1 && key->UnicodeChar <= 26)
        return key->UnicodeChar;
    if (key->UnicodeChar >= 0x20 && key->UnicodeChar <= 0x7e)
        return key->UnicodeChar;

    switch (key->ScanCode) {
    case SCAN_UP:
        return KEY_UP;
    case SCAN_DOWN:
        return KEY_DOWN;
    case SCAN_LEFT:
        return KEY_LEFT;
    case SCAN_RIGHT:
        return KEY_RIGHT;
    case SCAN_HOME:
        return KEY_HOME;
    case SCAN_END:
        return KEY_END;
    case SCAN_DELETE:
        return KEY_DELETE;
    case SCAN_PGUP:
        return KEY_PGUP;
    case SCAN_PGDN:
        return KEY_PGDN;
    case SCAN_F1:
        return KEY_F1;
    case SCAN_F2:
        return KEY_F2;
    case SCAN_F3:
        return KEY_F3;
    case SCAN_F4:
        return KEY_F4;
    case SCAN_F5:
        return KEY_F5;
    case SCAN_F6:
        return KEY_F6;
    case SCAN_F7:
        return KEY_F7;
    case SCAN_F8:
        return KEY_F8;
    case SCAN_ESC:
        return KEY_ESC;
    default:
        return 0;
    }
}

static void poll_keyboard(EFI_SYSTEM_TABLE *st)
{
    EFI_INPUT_KEY key;
    for (int i = 0; i < MAX_KEYS_PER_FRAME; i++) {
        if (EFI_ERROR(st->ConIn->ReadKeyStroke(st->ConIn, &key)))
            return;
        int translated = translate_key(&key);
        if (translated)
            wm_key(&g_wm, translated);
    }
}

/* Converts firmware pointer counts to pixels, keeping the remainder between frames. */
static int scale_motion(int32_t counts, uint64_t resolution, int64_t *remainder)
{
    if (resolution == 0)
        return counts;
    int64_t total = *remainder + (int64_t)counts * 8;
    int64_t pixels = total / (int64_t)resolution;
    *remainder = total - pixels * (int64_t)resolution;
    return (int)pixels;
}

static void poll_firmware_pointer(EFI_SIMPLE_POINTER_PROTOCOL *pointer)
{
    static int64_t remainder_x, remainder_y;
    EFI_SIMPLE_POINTER_STATE state;

    if (!pointer || EFI_ERROR(pointer->GetState(pointer, &state)))
        return;

    int buttons = (state.LeftButton ? BUTTON_LEFT : 0) | (state.RightButton ? BUTTON_RIGHT : 0);
    int dx = scale_motion(state.RelativeMovementX, pointer->Mode->ResolutionX, &remainder_x);
    int dy = scale_motion(state.RelativeMovementY, pointer->Mode->ResolutionY, &remainder_y);
    wm_pointer(&g_wm, dx, dy, buttons);
    if (state.RelativeMovementZ != 0)
        wm_wheel(&g_wm, state.RelativeMovementZ > 0 ? 1 : -1);
}

/* Days from 1970-01-01 to a civil date (Howard Hinnant's days_from_civil algorithm). */
static int64_t days_from_civil(int64_t year, int64_t month, int64_t day)
{
    year -= month <= 2;
    int64_t era = (year >= 0 ? year : year - 399) / 400;
    int64_t year_of_era = year - era * 400;
    int64_t day_of_year = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    int64_t day_of_era = year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
    return era * 146097 + day_of_era - 719468;
}

static void update_clock(EFI_SYSTEM_TABLE *st)
{
    EFI_TIME time;
    if (EFI_ERROR(st->RuntimeServices->GetTime(&time, NULL)))
        return;

    g_fs.now = fs_make_time(time.Year, time.Month, time.Day, time.Hour, time.Minute);

    /*
     * Certificate checks need UTC. UEFI defines local = UTC - TimeZone, and 2047
     * means unspecified, in which case the clock is assumed to be UTC.
     */
    int64_t days = days_from_civil(time.Year, time.Month, time.Day);
    int64_t seconds = time.Hour * 3600 + time.Minute * 60 + time.Second;
    if (time.TimeZone != 2047 && time.TimeZone >= -1440 && time.TimeZone <= 1440)
        seconds += (int64_t)time.TimeZone * 60;
    days += seconds >= 0 ? seconds / 86400 : (seconds - 86399) / 86400;
    seconds -= (seconds >= 0 ? seconds / 86400 : (seconds - 86399) / 86400) * 86400;
    g_net.clock_days = time.Year >= 2020 ? (uint32_t)(days + 719528) : 0;
    g_net.clock_seconds = (uint32_t)seconds;

    char clock[sizeof g_wm.clock];
    fs_format_time(g_fs.now, clock, sizeof clock);
    wm_set_clock(&g_wm, clock);
}

static void flush(const struct framebuffer *fb, const uint32_t *back, int width, int scale)
{
    if (g_wm.dirty_count == 0)
        return;
    wm_render(&g_wm);
    for (int i = 0; i < g_wm.dirty_count; i++)
        present(fb, back, width, scale, g_wm.dirty[i]);
    g_wm.dirty_count = 0;
}

EFI_STATUS desktop_run(EFI_SYSTEM_TABLE *st, EFI_HANDLE image, const struct framebuffer *fb)
{
    EFI_BOOT_SERVICES *bs = st->BootServices;
    int scale = ui_scale(fb->width, fb->height);
    int width = (int)fb->width / scale;
    int height = (int)fb->height / scale;
    UINTN pages = ((UINTN)width * (UINTN)height * 4 + 4095) / 4096;

    EFI_PHYSICAL_ADDRESS back_address = 0, wallpaper_address = 0;
    EFI_EVENT timer = NULL;
    EFI_STATUS status = bs->AllocatePages(AllocateAnyPages, EfiLoaderData, pages, &back_address);
    if (EFI_ERROR(status))
        return status;
    status = bs->AllocatePages(AllocateAnyPages, EfiLoaderData, pages, &wallpaper_address);
    if (EFI_ERROR(status))
        goto free_back;
    status = bs->CreateEvent(EVT_TIMER, 0, NULL, NULL, &timer);
    if (EFI_ERROR(status))
        goto free_wallpaper;
    status = bs->SetTimer(timer, TimerPeriodic, FRAME_100NS);
    if (EFI_ERROR(status))
        goto close_timer;

    uint32_t *back = (uint32_t *)(uintptr_t)back_address;
    uint32_t *wallpaper = (uint32_t *)(uintptr_t)wallpaper_address;
    splash_render(wallpaper, (uint32_t)width, (uint32_t)height);

    /* The legacy i8042 ports are touched only inside a VM. */
    int in_vm = cpu_in_vm();
    int ps2_active = in_vm && ps2_mouse_init(bs);

    EFI_GUID pointer_guid = EFI_SIMPLE_POINTER_PROTOCOL_GUID;
    EFI_SIMPLE_POINTER_PROTOCOL *pointer = NULL;
    if (EFI_ERROR(bs->HandleProtocol(st->ConsoleInHandle, &pointer_guid, (void **)&pointer)))
        pointer = NULL;
    if (pointer && (!pointer->Mode || EFI_ERROR(pointer->Reset(pointer, 0))))
        pointer = NULL;

    gather_sysinfo(st, fb, scale,
                   ps2_active ? "PS/2 mouse (VM)" : pointer ? "UEFI pointer" : "Keyboard only");

    struct canvas screen;
    canvas_init(&screen, back, width, height);
    fs_init(&g_fs);
    int storage = storage_open(st, &g_fs);
    wm_init(&g_wm, &screen, wallpaper, &g_info, &g_fs, in_vm ? debugcon_trace : NULL);
    g_wm.storage_ready = storage == STORAGE_EMPTY || storage == STORAGE_LOADED;

    /* This records handles only. The adapter stays idle until something uses the network. */
    struct net_driver driver;
    netdev_init(&driver, st, image);
    net_init(&g_net, &driver);
    g_wm.net = &g_net;

    gop_fill_rect(fb, 0, 0, fb->width, fb->height, 0, 0, 0);
    update_clock(st);
    flush(fb, back, width, scale);
    if (storage == STORAGE_LOADED) {
        trace_storage("storage:loaded:");
    } else if (storage == STORAGE_EMPTY) {
        wm_trace(&g_wm, "storage:empty");
    } else if (storage == STORAGE_UNREADABLE) {
        wm_trace(&g_wm, "storage:unreadable");
        shell_notice(&g_wm, "Saved USB files could not be read.");
    } else {
        wm_trace(&g_wm, "storage:none");
    }
    wm_trace(&g_wm, "ready");

    /* A new drive gets its first copy right away. */
    uint32_t saved_revision = storage == STORAGE_LOADED ? g_fs.revision : g_fs.revision - 1;
    uint32_t seen_revision = g_fs.revision;
    uint32_t last_change = 0;
    int save_failed = 0;

    st->ConIn->Reset(st->ConIn, 0);
    for (uint32_t frame = 1; !g_wm.exit_requested; frame++) {
        UINTN index;
        status = bs->WaitForEvent(1, &timer, &index);
        if (EFI_ERROR(status))
            break;

        net_poll(&g_net, frame * FRAME_MS);
        wm_tick(&g_wm);
        poll_keyboard(st);
        poll_firmware_pointer(pointer);
        if (ps2_active) {
            struct ps2_motion motion;
            if (ps2_mouse_poll(&motion)) {
                wm_pointer(&g_wm, motion.dx, motion.dy,
                           (motion.left ? BUTTON_LEFT : 0) | (motion.right ? BUTTON_RIGHT : 0));
                if (motion.wheel)
                    wm_wheel(&g_wm, motion.wheel);
            }
        }
        if (frame % CLOCK_FRAMES == 0)
            update_clock(st);

        if (g_wm.storage_ready) {
            if (g_fs.revision != seen_revision) {
                seen_revision = g_fs.revision;
                last_change = frame;
            }
            if (g_fs.revision != saved_revision && frame - last_change >= SAVE_DELAY_FRAMES) {
                saved_revision = g_fs.revision;
                save_failed = !storage_save(&g_fs);
                if (save_failed) {
                    wm_trace(&g_wm, "storage:failed");
                    shell_notice(&g_wm, "Could not save to the USB drive.");
                } else {
                    trace_storage("storage:saved:");
                }
            }
        }
        flush(fb, back, width, scale);
    }

    if (g_wm.storage_ready && (g_fs.revision != saved_revision || save_failed)) {
        if (storage_save(&g_fs)) {
            trace_storage("storage:saved:");
        } else {
            wm_trace(&g_wm, "storage:failed");
            st->ConOut->ClearScreen(st->ConOut);
            st->ConOut->OutputString(st->ConOut,
                                     (CHAR16 *)u"Warning: recent changes could not be saved to USB.\r\n");
            bs->Stall(4000000);
        }
    }
    storage_close();
    netdev_shutdown();
    if (ps2_active)
        ps2_mouse_shutdown(bs);
    bs->SetTimer(timer, TimerCancel, 0);
close_timer:
    bs->CloseEvent(timer);
free_wallpaper:
    bs->FreePages(wallpaper_address, pages);
free_back:
    bs->FreePages(back_address, pages);
    return status;
}
