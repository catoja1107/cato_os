#include "gop.h"

/* Converts a GOP's current mode to a framebuffer. Returns 0 if it is unusable. */
static int framebuffer_from_gop(const EFI_GRAPHICS_OUTPUT_PROTOCOL *gop, struct framebuffer *fb)
{
    if (!gop || !gop->Mode || !gop->Mode->Info)
        return 0;

    const EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE *mode = gop->Mode;
    const EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info = mode->Info;

    switch (info->PixelFormat) {
    case PixelRedGreenBlueReserved8BitPerColor:
        if (!pixel_layout_from_masks(0x000000ff, 0x0000ff00, 0x00ff0000, 0xff000000, &fb->layout))
            return 0;
        break;
    case PixelBlueGreenRedReserved8BitPerColor:
        if (!pixel_layout_from_masks(0x00ff0000, 0x0000ff00, 0x000000ff, 0xff000000, &fb->layout))
            return 0;
        break;
    case PixelBitMask:
        if (!pixel_layout_from_masks(info->PixelInformation.RedMask,
                                     info->PixelInformation.GreenMask,
                                     info->PixelInformation.BlueMask,
                                     info->PixelInformation.ReservedMask, &fb->layout))
            return 0;
        break;
    default:
        return 0; /* PixelBltOnly has no linear framebuffer. */
    }

    uint64_t required = (uint64_t)info->PixelsPerScanLine * info->VerticalResolution * 4;
    if (mode->FrameBufferBase == 0 || info->HorizontalResolution == 0 ||
        info->VerticalResolution == 0 || info->PixelsPerScanLine < info->HorizontalResolution ||
        mode->FrameBufferSize < required)
        return 0;

    fb->base = (volatile uint32_t *)(uintptr_t)mode->FrameBufferBase;
    fb->phys_base = mode->FrameBufferBase;
    fb->size = mode->FrameBufferSize;
    fb->width = info->HorizontalResolution;
    fb->height = info->VerticalResolution;
    fb->pixels_per_scanline = info->PixelsPerScanLine;
    fb->format = info->PixelFormat;
    return 1;
}

EFI_STATUS gop_open(EFI_SYSTEM_TABLE *st, struct framebuffer *fb)
{
    EFI_BOOT_SERVICES *bs = st->BootServices;
    EFI_GUID gop_guid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = NULL;

    /* The console's GOP is the display the user can see, which matters with two GPUs. */
    if (st->ConsoleOutHandle &&
        !EFI_ERROR(bs->HandleProtocol(st->ConsoleOutHandle, &gop_guid, (void **)&gop)) &&
        framebuffer_from_gop(gop, fb))
        return EFI_SUCCESS;

    UINTN count = 0;
    EFI_HANDLE *handles = NULL;
    EFI_STATUS status = bs->LocateHandleBuffer(ByProtocol, &gop_guid, NULL, &count, &handles);
    if (EFI_ERROR(status))
        return status;

    status = EFI_UNSUPPORTED;
    for (UINTN i = 0; i < count; i++) {
        gop = NULL;
        if (!EFI_ERROR(bs->HandleProtocol(handles[i], &gop_guid, (void **)&gop)) &&
            framebuffer_from_gop(gop, fb)) {
            status = EFI_SUCCESS;
            break;
        }
    }
    bs->FreePool(handles);
    return status;
}

void gop_fill_rect(const struct framebuffer *fb, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                   uint8_t red, uint8_t green, uint8_t blue)
{
    if (x >= fb->width || y >= fb->height)
        return;
    if (w > fb->width - x)
        w = fb->width - x;
    if (h > fb->height - y)
        h = fb->height - y;

    uint32_t color = pixel_pack(&fb->layout, red, green, blue);
    for (uint32_t row = y; row < y + h; row++) {
        volatile uint32_t *line = fb->base + (uint64_t)row * fb->pixels_per_scanline;
        for (uint32_t col = x; col < x + w; col++)
            line[col] = color;
    }
}

void gop_draw_test_pattern(const struct framebuffer *fb)
{
    static const uint8_t bars[7][3] = {
        { 255, 0, 0 },     /* red */
        { 0, 255, 0 },     /* green */
        { 0, 0, 255 },     /* blue */
        { 0, 255, 255 },   /* cyan */
        { 255, 0, 255 },   /* magenta */
        { 255, 255, 0 },   /* yellow */
        { 255, 255, 255 }, /* white */
    };

    uint32_t margin = fb->width / 20;
    uint32_t top = fb->height - fb->height / 4;
    uint32_t bottom = fb->height - fb->height / 20;
    uint32_t bar_width = (fb->width - 2 * margin) / 7;

    for (uint32_t i = 0; i < 7; i++)
        gop_fill_rect(fb, margin + i * bar_width, top, bar_width, bottom - top,
                      bars[i][0], bars[i][1], bars[i][2]);
}
