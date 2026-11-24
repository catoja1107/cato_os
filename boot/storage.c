#include "storage.h"

#include "fs_store.h"

#define MARKER "CATO-STORE-V1"
#define MARKER_LENGTH 13
#define SLOTS 2

static const CHAR16 *const SLOT_NAMES[SLOTS] = { u"SLOTA.DAT", u"SLOTB.DAT" };

static EFI_FILE_PROTOCOL *g_root;
static EFI_FILE_PROTOCOL *g_dir;
static int g_slot = -1;
static uint32_t g_generation;

/* These are large, so they live in static memory. */
static uint8_t g_buffer[FS_STORE_MAX];
static uint8_t g_verify[FS_STORE_MAX];
static struct fs g_scratch;

static int path_is_usb(const EFI_DEVICE_PATH_PROTOCOL *path)
{
    for (int guard = 0; guard < 64; guard++) {
        if (path->Type == EFI_DEVICE_PATH_END)
            return 0;
        if (path->Type == EFI_DEVICE_PATH_MESSAGING && path->SubType == EFI_DEVICE_PATH_MSG_USB)
            return 1;
        int length = path->Length[0] | (path->Length[1] << 8);
        if (length < 4)
            return 0;
        path = (const EFI_DEVICE_PATH_PROTOCOL *)((const uint8_t *)path + length);
    }
    return 0;
}

static int handle_is_usb(EFI_BOOT_SERVICES *bs, EFI_HANDLE handle)
{
    EFI_GUID path_guid = EFI_DEVICE_PATH_PROTOCOL_GUID;
    EFI_DEVICE_PATH_PROTOCOL *path = NULL;
    return !EFI_ERROR(bs->HandleProtocol(handle, &path_guid, (void **)&path)) && path &&
           path_is_usb(path);
}

static int is_writable_removable_usb(EFI_BOOT_SERVICES *bs, EFI_HANDLE handle)
{
    EFI_GUID block_guid = EFI_BLOCK_IO_PROTOCOL_GUID;
    EFI_BLOCK_IO_PROTOCOL *block = NULL;

    if (!handle_is_usb(bs, handle))
        return 0;
    if (EFI_ERROR(bs->HandleProtocol(handle, &block_guid, (void **)&block)) || !block ||
        !block->Media)
        return 0;
    return block->Media->RemovableMedia && block->Media->MediaPresent && !block->Media->ReadOnly;
}

/*
 * Firmware may start only the drive it booted from, so start drivers for other USB
 * devices too. Handles whose device path does not go through USB are skipped.
 */
static void connect_usb_storage(EFI_BOOT_SERVICES *bs)
{
    EFI_GUID path_guid = EFI_DEVICE_PATH_PROTOCOL_GUID;
    UINTN count = 0;
    EFI_HANDLE *handles = NULL;

    if (EFI_ERROR(bs->LocateHandleBuffer(ByProtocol, &path_guid, NULL, &count, &handles)))
        return;
    for (UINTN i = 0; i < count; i++) {
        if (handle_is_usb(bs, handles[i]))
            bs->ConnectController(handles[i], NULL, NULL, 1);
    }
    bs->FreePool(handles);
}

/* Checks for the marker using read-only opens. */
static int volume_has_marker(EFI_FILE_PROTOCOL *root)
{
    EFI_FILE_PROTOCOL *dir = NULL;
    EFI_FILE_PROTOCOL *file = NULL;
    char text[MARKER_LENGTH];
    UINTN size = MARKER_LENGTH;
    int found = 0;

    if (EFI_ERROR(root->Open(root, &dir, (CHAR16 *)u"CATOOS", EFI_FILE_MODE_READ, 0)))
        return 0;
    if (!EFI_ERROR(dir->Open(dir, &file, (CHAR16 *)u"STORE.ID", EFI_FILE_MODE_READ, 0))) {
        if (!EFI_ERROR(file->Read(file, &size, text)) && size == MARKER_LENGTH)
            found = __builtin_memcmp(text, MARKER, MARKER_LENGTH) == 0;
        file->Close(file);
    }
    dir->Close(dir);
    return found;
}

/* Finds the single approved volume. Two marked drives are refused as ambiguous. */
static EFI_FILE_PROTOCOL *find_approved_volume(EFI_BOOT_SERVICES *bs)
{
    EFI_GUID fs_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    UINTN count = 0;
    EFI_HANDLE *handles = NULL;
    EFI_FILE_PROTOCOL *approved = NULL;
    int matches = 0;

    connect_usb_storage(bs);
    if (EFI_ERROR(bs->LocateHandleBuffer(ByProtocol, &fs_guid, NULL, &count, &handles)))
        return NULL;

    for (UINTN i = 0; i < count; i++) {
        /* This check runs first, so internal drives are never opened at all. */
        if (!is_writable_removable_usb(bs, handles[i]))
            continue;

        EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *volume = NULL;
        EFI_FILE_PROTOCOL *root = NULL;
        if (EFI_ERROR(bs->HandleProtocol(handles[i], &fs_guid, (void **)&volume)) ||
            EFI_ERROR(volume->OpenVolume(volume, &root)))
            continue;

        if (volume_has_marker(root)) {
            matches++;
            if (approved)
                approved->Close(approved);
            approved = root;
        } else {
            root->Close(root);
        }
    }
    bs->FreePool(handles);

    if (matches != 1) {
        if (approved)
            approved->Close(approved);
        return NULL;
    }
    return approved;
}

/* Returns 1 for a valid slot. *exists is 0 when the slot has never held data. */
static int read_slot(int slot, uint8_t *out, uint32_t *generation, int *length, int *exists)
{
    EFI_FILE_PROTOCOL *file = NULL;
    UINTN size = FS_STORE_MAX;

    *exists = 0;
    if (EFI_ERROR(g_dir->Open(g_dir, &file, (CHAR16 *)SLOT_NAMES[slot], EFI_FILE_MODE_READ, 0)))
        return 0;
    EFI_STATUS status = file->Read(file, &size, out);
    file->Close(file);
    if (EFI_ERROR(status))
        return 0;

    *exists = size >= FS_STORE_HEADER;
    return fs_store_check(out, (int)size, generation, length);
}

static int load_saved(struct fs *fs)
{
    uint32_t generation[SLOTS] = { 0, 0 };
    int length[SLOTS] = { 0, 0 };
    int exists[SLOTS];
    int valid[SLOTS];
    uint8_t *const buffers[SLOTS] = { g_buffer, g_verify };

    for (int slot = 0; slot < SLOTS; slot++)
        valid[slot] = read_slot(slot, buffers[slot], &generation[slot], &length[slot],
                                &exists[slot]);
    if (!exists[0] && !exists[1])
        return STORAGE_EMPTY;

    /* Try the newest valid copy first, then fall back to the older one. */
    int first = valid[0] && (!valid[1] || (int32_t)(generation[0] - generation[1]) > 0) ? 0 : 1;
    for (int attempt = 0; attempt < SLOTS; attempt++) {
        int slot = attempt == 0 ? first : 1 - first;
        if (valid[slot] && fs_store_load(&g_scratch, buffers[slot], length[slot])) {
            *fs = g_scratch;
            g_slot = slot;
            g_generation = generation[slot];
            return STORAGE_LOADED;
        }
    }
    return STORAGE_UNREADABLE;
}

int storage_open(EFI_SYSTEM_TABLE *st, struct fs *fs)
{
    g_root = find_approved_volume(st->BootServices);
    g_dir = NULL;
    g_slot = -1;
    g_generation = 0;
    if (!g_root)
        return STORAGE_NONE;

    if (EFI_ERROR(g_root->Open(g_root, &g_dir, (CHAR16 *)u"CATOOS",
                               EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0))) {
        g_dir = NULL;
        storage_close();
        return STORAGE_NONE;
    }

    int result = load_saved(fs);
    if (result == STORAGE_UNREADABLE)
        storage_close(); /* Leave damaged data untouched so it can be recovered. */
    return result;
}

int storage_save(const struct fs *fs)
{
    if (!g_dir)
        return 0;

    /* Always overwrite the older copy, so the newest good copy survives a failed write. */
    int slot = g_slot == 0 ? 1 : 0;
    uint32_t generation = g_generation + 1;
    int length = fs_store_save(fs, generation, g_buffer, FS_STORE_MAX);
    if (length == 0)
        return 0;

    EFI_FILE_PROTOCOL *file = NULL;
    if (EFI_ERROR(g_dir->Open(g_dir, &file, (CHAR16 *)SLOT_NAMES[slot],
                              EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0)))
        return 0;

    UINTN size = (UINTN)length;
    int ok = !EFI_ERROR(file->SetPosition(file, 0)) &&
             !EFI_ERROR(file->Write(file, &size, g_buffer)) && size == (UINTN)length &&
             !EFI_ERROR(file->Flush(file));

    /* Read the data back before trusting it. */
    if (ok) {
        size = (UINTN)length;
        ok = !EFI_ERROR(file->SetPosition(file, 0)) &&
             !EFI_ERROR(file->Read(file, &size, g_verify)) && size == (UINTN)length &&
             __builtin_memcmp(g_buffer, g_verify, (unsigned)length) == 0;
    }
    file->Close(file);
    if (!ok)
        return 0;

    g_slot = slot;
    g_generation = generation;
    return 1;
}

void storage_close(void)
{
    if (g_dir) {
        g_dir->Flush(g_dir);
        g_dir->Close(g_dir);
        g_dir = NULL;
    }
    if (g_root) {
        g_root->Close(g_root);
        g_root = NULL;
    }
}
