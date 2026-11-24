/*
 * On-disk image of the filesystem. The loader treats saved data as untrusted,
 * because a drive can be corrupted or edited on another computer.
 */
#ifndef CATO_FS_STORE_H
#define CATO_FS_STORE_H

#include "fs.h"

#define FS_STORE_HEADER 32
#define FS_STORE_RECORD_MAX 78
#define FS_STORE_MAX \
    (FS_STORE_HEADER + FS_MAX_NODES * FS_STORE_RECORD_MAX + FS_BLOCKS * FS_BLOCK_SIZE)

/* Standard IEEE CRC-32. */
uint32_t fs_crc32(const uint8_t *data, int length);
/* Serializes fs. Returns the image length, or 0 if it does not fit. */
int fs_store_save(const struct fs *fs, uint32_t generation, uint8_t *out, int capacity);
/* Verifies the header and checksums. Returns 1 and the image's generation and length. */
int fs_store_check(const uint8_t *data, int length, uint32_t *generation, int *total);
/* Rebuilds fs from a verified image. Returns 0, leaving fs unusable, if anything is invalid. */
int fs_store_load(struct fs *fs, const uint8_t *data, int length);

#endif
