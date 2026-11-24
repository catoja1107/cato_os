/*
 * In-memory filesystem for Cato Desktop. It never touches a disk, so all
 * contents are lost when the machine resets. This code has no firmware
 * dependencies and is unit tested on the host.
 */
#ifndef CATO_FS_H
#define CATO_FS_H

#include <stdint.h>

#define FS_MAX_NODES 256
#define FS_NAME_MAX 32 /* Includes the terminating NUL. */
#define FS_BLOCK_SIZE 512
#define FS_BLOCKS 4096 /* 2 MiB of file data. */
#define FS_NONE (-1)
#define FS_ROOT 0

enum fs_type { FS_FREE, FS_DIR, FS_FILE };

enum fs_status {
    FS_OK = 0,
    FS_ERR_EXISTS = -1,
    FS_ERR_NOT_FOUND = -2,
    FS_ERR_NO_SPACE = -3,
    FS_ERR_BAD_NAME = -4,
    FS_ERR_NOT_DIR = -5,
    FS_ERR_IS_DIR = -6,
    FS_ERR_PROTECTED = -7,
    FS_ERR_CYCLE = -8,
};

struct fs_node {
    uint8_t type;
    uint8_t is_protected; /* Standard folders cannot be renamed, moved, or deleted. */
    int16_t parent;
    int16_t first_child;
    int16_t next_sibling;
    int16_t trash_origin; /* Folder to restore to, or FS_NONE. */
    int32_t first_block;
    uint32_t size;
    uint32_t modified;
    char name[FS_NAME_MAX];
    char trash_name[FS_NAME_MAX]; /* Original name while in the Trash. */
};

struct fs {
    struct fs_node nodes[FS_MAX_NODES];
    int32_t block_next[FS_BLOCKS];
    int32_t free_block;
    int free_blocks;
    int free_nodes;
    uint32_t now;
    uint32_t revision; /* Changes whenever the contents change. */
    int desktop;
    int documents;
    int downloads;
    int pictures;
    int trash;
    uint8_t blocks[FS_BLOCKS][FS_BLOCK_SIZE];
};

/* Creates the standard folders and starter files. */
void fs_init(struct fs *fs);
/* Creates an empty tree holding only the root. Used when loading saved data. */
void fs_format(struct fs *fs);
/* Adds a node without policy checks. Only the loader should call this. */
int fs_add_node(struct fs *fs, int parent, const char *name, int type);

uint32_t fs_make_time(int year, int month, int day, int hour, int minute);
/* Writes "YYYY-MM-DD HH:MM", or "-" for an unset time. */
void fs_format_time(uint32_t time, char *out, int capacity);
const char *fs_error_text(int status);

int fs_valid_name(const char *name);
int fs_is_dir(const struct fs *fs, int node);
int fs_is_file(const struct fs *fs, int node);
/* True if node is ancestor or is inside it. */
int fs_is_within(const struct fs *fs, int node, int ancestor);

int fs_lookup(const struct fs *fs, int dir, const char *name);
/* Resolves absolute or relative paths, including "." and "..". */
int fs_resolve(const struct fs *fs, int cwd, const char *path);
void fs_path(const struct fs *fs, int node, char *out, int capacity);
/* Lists children with folders first, then by case-insensitive name. */
int fs_list(const struct fs *fs, int dir, int *out, int max);
/* Produces "name", "name (2)", "name (3).txt", and so on, unused in dir. */
void fs_unique_name(const struct fs *fs, int dir, const char *name, char *out, int capacity);

/* These return a node id or FS_OK on success, and a negative fs_status on failure. */
int fs_create(struct fs *fs, int parent, const char *name, int type);
int fs_write(struct fs *fs, int node, const char *data, int length);
int fs_read(const struct fs *fs, int node, int offset, char *out, int length);
int fs_rename(struct fs *fs, int node, const char *name);
int fs_move(struct fs *fs, int node, int dir);
int fs_copy(struct fs *fs, int node, int dir);
int fs_delete(struct fs *fs, int node);
int fs_trash(struct fs *fs, int node);
int fs_restore(struct fs *fs, int node);
int fs_empty_trash(struct fs *fs);

uint32_t fs_used_bytes(const struct fs *fs);

#endif
