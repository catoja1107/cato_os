/*
 * Saves the filesystem to the one approved USB drive.
 *
 * A volume qualifies only if its device path goes through USB, its media is
 * removable and writable, and it holds the marker file \CATOOS\STORE.ID. Other
 * volumes, including every internal drive, are never opened. All writes go to
 * two files in \CATOOS through the firmware's file protocol, with no raw block I/O.
 */
#ifndef CATO_BOOT_STORAGE_H
#define CATO_BOOT_STORAGE_H

#include "efi.h"
#include "fs.h"

enum storage_result {
    STORAGE_NONE,       /* No approved drive is present. */
    STORAGE_EMPTY,      /* The drive is approved and holds no saved files yet. */
    STORAGE_LOADED,     /* Saved files were loaded. */
    STORAGE_UNREADABLE, /* Saved data is damaged, so nothing will be overwritten. */
};

/* Finds the approved drive and loads saved files into fs when valid. */
int storage_open(EFI_SYSTEM_TABLE *st, struct fs *fs);
/* Writes a new verified copy, keeping the previous copy intact. Returns 1 on success. */
int storage_save(const struct fs *fs);
void storage_close(void);

#endif
