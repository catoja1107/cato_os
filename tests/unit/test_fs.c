/* Host unit tests for the in-memory filesystem. Built with ASan and UBSan. */
#include <stdio.h>
#include <string.h>

#include "fs.h"
#include "fs_store.h"

static int failures;
static struct fs g_fs;
static struct fs g_loaded;
static uint8_t g_image[FS_STORE_MAX];
static uint8_t g_mutated[FS_STORE_MAX];

#define CHECK(condition)                                                 \
    do {                                                                 \
        if (!(condition)) {                                              \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);  \
            failures++;                                                  \
        }                                                                \
    } while (0)

static struct fs *fresh(void)
{
    fs_init(&g_fs);
    return &g_fs;
}

static int read_equals(const struct fs *fs, int node, const char *expected)
{
    char buffer[4096];
    int length = fs_read(fs, node, 0, buffer, sizeof buffer);
    return length == (int)strlen(expected) && memcmp(buffer, expected, (size_t)length) == 0;
}

static void test_standard_layout(void)
{
    struct fs *fs = fresh();
    int names[FS_MAX_NODES];

    CHECK(fs_list(fs, FS_ROOT, names, FS_MAX_NODES) == 5);
    CHECK(fs_resolve(fs, FS_ROOT, "/Documents") == fs->documents);
    CHECK(fs_is_file(fs, fs_resolve(fs, FS_ROOT, "/Desktop/Welcome.txt")));
    CHECK(fs_rename(fs, fs->documents, "Other") == FS_ERR_PROTECTED);
    CHECK(fs_trash(fs, fs->documents) == FS_ERR_PROTECTED);
    CHECK(fs_delete(fs, fs->trash) == FS_ERR_PROTECTED);
    CHECK(fs_delete(fs, FS_ROOT) == FS_ERR_PROTECTED);
    CHECK(fs_move(fs, fs->desktop, fs->documents) == FS_ERR_PROTECTED);
}

static void test_names(void)
{
    char too_long[FS_NAME_MAX + 1];
    memset(too_long, 'a', FS_NAME_MAX);
    too_long[FS_NAME_MAX] = 0;

    CHECK(fs_valid_name("Report 1.txt"));
    CHECK(!fs_valid_name(""));
    CHECK(!fs_valid_name("."));
    CHECK(!fs_valid_name(".."));
    CHECK(!fs_valid_name("a/b"));
    CHECK(!fs_valid_name(" leading"));
    CHECK(!fs_valid_name("trailing "));
    CHECK(!fs_valid_name("quo\"te"));
    CHECK(!fs_valid_name(too_long));
    too_long[FS_NAME_MAX - 1] = 0;
    CHECK(fs_valid_name(too_long));
}

static void test_paths(void)
{
    struct fs *fs = fresh();
    char path[64];
    int a = fs_create(fs, fs->documents, "a", FS_DIR);
    int b = fs_create(fs, a, "b c", FS_DIR);

    fs_path(fs, b, path, sizeof path);
    CHECK(strcmp(path, "/Documents/a/b c") == 0);
    fs_path(fs, FS_ROOT, path, sizeof path);
    CHECK(strcmp(path, "/") == 0);

    CHECK(fs_resolve(fs, a, "b c") == b);
    CHECK(fs_resolve(fs, b, "..") == a);
    CHECK(fs_resolve(fs, b, "../..") == fs->documents);
    CHECK(fs_resolve(fs, b, "./../b c/.") == b);
    CHECK(fs_resolve(fs, FS_ROOT, "../../Documents") == fs->documents);
    CHECK(fs_resolve(fs, FS_ROOT, "//Documents//a/") == a);
    CHECK(fs_resolve(fs, FS_ROOT, "/missing") == FS_ERR_NOT_FOUND);
    CHECK(fs_resolve(fs, FS_ROOT, "/Desktop/Welcome.txt/x") == FS_ERR_NOT_DIR);
}

static void test_read_write(void)
{
    struct fs *fs = fresh();
    int node = fs_create(fs, fs->documents, "data.bin", FS_FILE);
    static char big[FS_BLOCK_SIZE * 3 + 17];
    static char back[sizeof big];

    for (size_t i = 0; i < sizeof big; i++)
        big[i] = (char)(i * 7);
    int before = fs->free_blocks;
    CHECK(fs_write(fs, node, big, sizeof big) == FS_OK);
    CHECK(fs->free_blocks == before - 4);
    CHECK(fs_read(fs, node, 0, back, sizeof back) == (int)sizeof big);
    CHECK(memcmp(big, back, sizeof big) == 0);

    /* Reads may start inside a block and cross block boundaries. */
    CHECK(fs_read(fs, node, FS_BLOCK_SIZE - 3, back, 10) == 10);
    CHECK(memcmp(big + FS_BLOCK_SIZE - 3, back, 10) == 0);
    CHECK(fs_read(fs, node, sizeof big, back, 10) == 0);
    CHECK(fs_read(fs, node, sizeof big - 2, back, 10) == 2);

    /* Overwriting with less data releases blocks. */
    CHECK(fs_write(fs, node, "short", 5) == FS_OK);
    CHECK(fs->free_blocks == before - 1);
    CHECK(read_equals(fs, node, "short"));
    CHECK(fs_write(fs, node, "", 0) == FS_OK);
    CHECK(fs->free_blocks == before);
    CHECK(fs->nodes[node].size == 0);

    CHECK(fs_write(fs, fs->documents, "x", 1) == FS_ERR_IS_DIR);
    CHECK(fs_read(fs, fs->documents, 0, back, 1) == FS_ERR_IS_DIR);
}

static void test_out_of_space(void)
{
    struct fs *fs = fresh();
    int first = fs_create(fs, fs->documents, "first", FS_FILE);
    int second = fs_create(fs, fs->documents, "second", FS_FILE);
    static char data[FS_BLOCK_SIZE * FS_BLOCKS];

    CHECK(fs_write(fs, first, "keep me", 7) == FS_OK);
    int available = fs->free_blocks * FS_BLOCK_SIZE;
    CHECK(fs_write(fs, second, data, available) == FS_OK);
    CHECK(fs->free_blocks == 0);

    /* A failed write leaves the original contents alone. */
    CHECK(fs_write(fs, first, data, FS_BLOCK_SIZE * 2) == FS_ERR_NO_SPACE);
    CHECK(read_equals(fs, first, "keep me"));

    /* A copy that cannot fit leaves nothing behind. */
    int count = fs->free_nodes;
    CHECK(fs_copy(fs, second, fs->downloads) == FS_ERR_NO_SPACE);
    CHECK(fs->free_nodes == count);
    CHECK(fs_lookup(fs, fs->downloads, "second") == FS_ERR_NOT_FOUND);

    CHECK(fs_delete(fs, second) == FS_OK);
    CHECK(fs->free_blocks > 0);
}

static void test_node_exhaustion(void)
{
    struct fs *fs = fresh();
    char name[16];
    int created = 0;

    for (int i = 0; i < FS_MAX_NODES + 8; i++) {
        snprintf(name, sizeof name, "f%d", i);
        if (fs_create(fs, fs->downloads, name, FS_FILE) >= 0)
            created++;
    }
    CHECK(created == FS_MAX_NODES - 8);
    CHECK(fs->free_nodes == 0);
    CHECK(fs_create(fs, fs->downloads, "more", FS_FILE) == FS_ERR_NO_SPACE);
}

static void test_create_and_rename(void)
{
    struct fs *fs = fresh();
    int node = fs_create(fs, fs->documents, "one.txt", FS_FILE);

    CHECK(fs_create(fs, fs->documents, "one.txt", FS_FILE) == FS_ERR_EXISTS);
    CHECK(fs_create(fs, fs->documents, "a/b", FS_FILE) == FS_ERR_BAD_NAME);
    CHECK(fs_create(fs, node, "x", FS_FILE) == FS_ERR_NOT_DIR);
    CHECK(fs_create(fs, fs->trash, "x", FS_FILE) == FS_ERR_PROTECTED);

    int other = fs_create(fs, fs->documents, "two.txt", FS_FILE);
    CHECK(fs_rename(fs, other, "one.txt") == FS_ERR_EXISTS);
    CHECK(fs_rename(fs, other, "bad/name") == FS_ERR_BAD_NAME);
    CHECK(fs_rename(fs, other, "two.txt") == FS_OK);
    CHECK(fs_rename(fs, other, "three.txt") == FS_OK);
    CHECK(fs_lookup(fs, fs->documents, "three.txt") == other);
}

static void test_list_order(void)
{
    struct fs *fs = fresh();
    int items[8];

    fs_create(fs, fs->downloads, "zebra.txt", FS_FILE);
    fs_create(fs, fs->downloads, "Apple.txt", FS_FILE);
    fs_create(fs, fs->downloads, "mango", FS_DIR);
    fs_create(fs, fs->downloads, "Berry", FS_DIR);

    CHECK(fs_list(fs, fs->downloads, items, 8) == 4);
    CHECK(strcmp(fs->nodes[items[0]].name, "Berry") == 0);
    CHECK(strcmp(fs->nodes[items[1]].name, "mango") == 0);
    CHECK(strcmp(fs->nodes[items[2]].name, "Apple.txt") == 0);
    CHECK(strcmp(fs->nodes[items[3]].name, "zebra.txt") == 0);
    CHECK(fs_list(fs, fs->downloads, items, 2) == 2);
}

static void test_unique_names(void)
{
    struct fs *fs = fresh();
    char name[FS_NAME_MAX];

    fs_unique_name(fs, fs->downloads, "a.txt", name, sizeof name);
    CHECK(strcmp(name, "a.txt") == 0);
    fs_create(fs, fs->downloads, "a.txt", FS_FILE);
    fs_unique_name(fs, fs->downloads, "a.txt", name, sizeof name);
    CHECK(strcmp(name, "a (2).txt") == 0);
    fs_create(fs, fs->downloads, name, FS_FILE);
    fs_unique_name(fs, fs->downloads, "a.txt", name, sizeof name);
    CHECK(strcmp(name, "a (3).txt") == 0);

    fs_create(fs, fs->downloads, ".hidden", FS_FILE);
    fs_unique_name(fs, fs->downloads, ".hidden", name, sizeof name);
    CHECK(strcmp(name, ".hidden (2)") == 0);

    /* A name at the length limit still yields a valid unique name. */
    char longest[FS_NAME_MAX];
    memset(longest, 'x', FS_NAME_MAX - 1);
    longest[FS_NAME_MAX - 1] = 0;
    fs_create(fs, fs->downloads, longest, FS_FILE);
    fs_unique_name(fs, fs->downloads, longest, name, sizeof name);
    CHECK(fs_valid_name(name) && strcmp(name, longest) != 0);
    CHECK(fs_create(fs, fs->downloads, name, FS_FILE) >= 0);
}

static void test_move(void)
{
    struct fs *fs = fresh();
    int a = fs_create(fs, fs->documents, "a", FS_DIR);
    int b = fs_create(fs, a, "b", FS_DIR);
    int file = fs_create(fs, b, "f.txt", FS_FILE);

    CHECK(fs_move(fs, a, b) == FS_ERR_CYCLE);
    CHECK(fs_move(fs, a, a) == FS_ERR_CYCLE);
    CHECK(fs_move(fs, file, file) == FS_ERR_NOT_DIR);
    CHECK(fs_move(fs, file, fs->documents) == FS_OK);
    CHECK(fs->nodes[file].parent == fs->documents);
    CHECK(fs_lookup(fs, b, "f.txt") == FS_ERR_NOT_FOUND);

    /* A name collision produces a unique name instead of overwriting. */
    int other = fs_create(fs, fs->downloads, "f.txt", FS_FILE);
    CHECK(fs_move(fs, file, fs->downloads) == FS_OK);
    CHECK(strcmp(fs->nodes[file].name, "f (2).txt") == 0);
    CHECK(fs_lookup(fs, fs->downloads, "f.txt") == other);
}

static void test_copy_tree(void)
{
    struct fs *fs = fresh();
    int a = fs_create(fs, fs->documents, "a", FS_DIR);
    int sub = fs_create(fs, a, "sub", FS_DIR);
    int file = fs_create(fs, sub, "f.txt", FS_FILE);
    fs_write(fs, file, "hello", 5);

    CHECK(fs_copy(fs, a, sub) == FS_ERR_CYCLE);
    CHECK(fs_copy(fs, fs->documents, fs->downloads) == FS_ERR_PROTECTED);
    CHECK(fs_copy(fs, file, fs->trash) == FS_ERR_PROTECTED);

    int copy = fs_copy(fs, a, fs->downloads);
    CHECK(copy >= 0 && copy != a);
    int copied_file = fs_resolve(fs, FS_ROOT, "/Downloads/a/sub/f.txt");
    CHECK(copied_file >= 0 && copied_file != file);
    CHECK(read_equals(fs, copied_file, "hello"));

    /* The copy is independent of the original. */
    fs_write(fs, copied_file, "changed", 7);
    CHECK(read_equals(fs, file, "hello"));

    /* Copying into the same folder creates a new name. */
    int same = fs_copy(fs, file, sub);
    CHECK(same >= 0 && strcmp(fs->nodes[same].name, "f (2).txt") == 0);
}

static void test_delete_releases_everything(void)
{
    struct fs *fs = fresh();
    int nodes = fs->free_nodes;
    int blocks = fs->free_blocks;
    int a = fs_create(fs, fs->documents, "a", FS_DIR);
    int b = fs_create(fs, a, "b", FS_DIR);
    int file = fs_create(fs, b, "f", FS_FILE);
    fs_write(fs, file, "data", 4);

    CHECK(fs_delete(fs, a) == FS_OK);
    CHECK(fs->free_nodes == nodes && fs->free_blocks == blocks);
    CHECK(fs_lookup(fs, fs->documents, "a") == FS_ERR_NOT_FOUND);
    CHECK(fs_delete(fs, a) == FS_ERR_NOT_FOUND);
}

static void test_trash(void)
{
    struct fs *fs = fresh();
    int a = fs_create(fs, fs->documents, "a.txt", FS_FILE);
    int second = fs_create(fs, fs->downloads, "a.txt", FS_FILE);

    CHECK(fs_trash(fs, a) == FS_OK);
    CHECK(fs->nodes[a].parent == fs->trash);
    CHECK(fs_trash(fs, a) == FS_ERR_PROTECTED);
    CHECK(fs_rename(fs, a, "x") == FS_ERR_PROTECTED);

    /* A second item with the same name gets a Trash-unique name. */
    CHECK(fs_trash(fs, second) == FS_OK);
    CHECK(strcmp(fs->nodes[second].name, "a (2).txt") == 0);

    /* Restore returns items to their folder under their original names. */
    CHECK(fs_restore(fs, second) == FS_OK);
    CHECK(fs->nodes[second].parent == fs->downloads);
    CHECK(strcmp(fs->nodes[second].name, "a.txt") == 0);

    /* A conflicting new file does not get overwritten by a restore. */
    int replacement = fs_create(fs, fs->documents, "a.txt", FS_FILE);
    CHECK(fs_restore(fs, a) == FS_OK);
    CHECK(fs_lookup(fs, fs->documents, "a.txt") == replacement);
    CHECK(strcmp(fs->nodes[a].name, "a (2).txt") == 0);
    CHECK(fs_restore(fs, a) == FS_ERR_NOT_FOUND);
}

static void test_trash_lost_origin(void)
{
    struct fs *fs = fresh();
    int folder = fs_create(fs, fs->downloads, "folder", FS_DIR);
    int file = fs_create(fs, folder, "f.txt", FS_FILE);

    CHECK(fs_trash(fs, file) == FS_OK);
    CHECK(fs_delete(fs, folder) == FS_OK);

    /* The original folder is gone, so the item goes to Documents. */
    CHECK(fs_restore(fs, file) == FS_OK);
    CHECK(fs->nodes[file].parent == fs->documents);

    /* An origin that is itself in the Trash also falls back to Documents. */
    int parent = fs_create(fs, fs->downloads, "parent", FS_DIR);
    int child = fs_create(fs, parent, "child.txt", FS_FILE);
    CHECK(fs_trash(fs, child) == FS_OK);
    CHECK(fs_trash(fs, parent) == FS_OK);
    CHECK(fs_restore(fs, child) == FS_OK);
    CHECK(fs->nodes[child].parent == fs->documents);
}

static void test_move_out_of_trash(void)
{
    struct fs *fs = fresh();
    int file = fs_create(fs, fs->documents, "f.txt", FS_FILE);

    CHECK(fs_move(fs, file, fs->trash) == FS_OK);
    CHECK(fs->nodes[file].trash_origin == fs->documents);
    CHECK(fs_move(fs, file, fs->downloads) == FS_OK);
    CHECK(fs->nodes[file].parent == fs->downloads);
    CHECK(fs->nodes[file].trash_origin == FS_NONE);
}

static void test_empty_trash(void)
{
    struct fs *fs = fresh();
    int nodes = fs->free_nodes;
    int blocks = fs->free_blocks;
    int folder = fs_create(fs, fs->documents, "folder", FS_DIR);
    int file = fs_create(fs, folder, "f.txt", FS_FILE);
    int other = fs_create(fs, fs->documents, "g.txt", FS_FILE);
    fs_write(fs, file, "x", 1);
    fs_write(fs, other, "y", 1);

    fs_trash(fs, folder);
    fs_trash(fs, other);
    CHECK(fs_empty_trash(fs) == FS_OK);
    CHECK(fs->nodes[fs->trash].first_child == FS_NONE);
    CHECK(fs->free_nodes == nodes && fs->free_blocks == blocks);
}

static void test_time(void)
{
    char text[17];
    uint32_t time = fs_make_time(2026, 11, 23, 2, 9);

    fs_format_time(time, text, sizeof text);
    CHECK(strcmp(text, "2026-11-23 02:09") == 0);
    fs_format_time(0, text, sizeof text);
    CHECK(strcmp(text, "-") == 0);
}

static void test_crc32(void)
{
    /* The standard CRC-32 check value. */
    CHECK(fs_crc32((const uint8_t *)"123456789", 9) == 0xcbf43926u);
    CHECK(fs_crc32((const uint8_t *)"", 0) == 0);
}

static void test_revision_tracks_changes(void)
{
    struct fs *fs = fresh();
    uint32_t revision = fs->revision;

    int node = fs_create(fs, fs->documents, "a.txt", FS_FILE);
    CHECK(fs->revision != revision);
    revision = fs->revision;
    fs_write(fs, node, "x", 1);
    CHECK(fs->revision != revision);
    revision = fs->revision;
    fs_rename(fs, node, "b.txt");
    CHECK(fs->revision != revision);
    revision = fs->revision;
    fs_move(fs, node, fs->downloads);
    CHECK(fs->revision != revision);
    revision = fs->revision;
    fs_trash(fs, node);
    CHECK(fs->revision != revision);
    revision = fs->revision;
    fs_restore(fs, node);
    CHECK(fs->revision != revision);
    revision = fs->revision;
    fs_delete(fs, node);
    CHECK(fs->revision != revision);

    /* Rejected operations do not count as changes. */
    revision = fs->revision;
    CHECK(fs_create(fs, fs->documents, "bad/name", FS_FILE) < 0);
    CHECK(fs_rename(fs, fs->documents, "x") < 0);
    CHECK(fs->revision == revision);
}

static void test_store_round_trip(void)
{
    struct fs *fs = fresh();
    fs->now = fs_make_time(2026, 11, 23, 12, 30);
    int work = fs_create(fs, fs->documents, "Work", FS_DIR);
    int sub = fs_create(fs, work, "Deep Folder", FS_DIR);
    int file = fs_create(fs, sub, "report (1).txt", FS_FILE);
    int empty = fs_create(fs, work, "empty.txt", FS_FILE);
    static char big[FS_BLOCK_SIZE * 5 + 3];
    for (size_t i = 0; i < sizeof big; i++)
        big[i] = (char)(i * 13);
    CHECK(fs_write(fs, file, big, sizeof big) == FS_OK);

    int trashed = fs_create(fs, fs->downloads, "old.txt", FS_FILE);
    fs_write(fs, trashed, "old", 3);
    fs_trash(fs, trashed);
    (void)empty;

    int length = fs_store_save(fs, 42, g_image, sizeof g_image);
    CHECK(length > FS_STORE_HEADER);

    uint32_t generation = 0;
    int total = 0;
    CHECK(fs_store_check(g_image, length, &generation, &total));
    CHECK(generation == 42 && total == length);

    /* Extra bytes after the image, as in a reused slot file, are ignored. */
    CHECK(fs_store_load(&g_loaded, g_image, length));
    struct fs *l = &g_loaded;
    CHECK(l->free_nodes == fs->free_nodes && l->free_blocks == fs->free_blocks);

    static char back[sizeof big];
    int loaded_file = fs_resolve(l, FS_ROOT, "/Documents/Work/Deep Folder/report (1).txt");
    CHECK(fs_is_file(l, loaded_file));
    CHECK(fs_read(l, loaded_file, 0, back, sizeof back) == (int)sizeof big);
    CHECK(memcmp(big, back, sizeof big) == 0);
    CHECK(l->nodes[loaded_file].modified == fs->nodes[file].modified);
    CHECK(fs_is_file(l, fs_resolve(l, FS_ROOT, "/Documents/Work/empty.txt")));
    CHECK(l->nodes[l->documents].is_protected && l->nodes[l->trash].is_protected);

    /* A trashed item still restores to its original folder after a reload. */
    int loaded_trashed = fs_lookup(l, l->trash, "old.txt");
    CHECK(loaded_trashed >= 0);
    CHECK(fs_restore(l, loaded_trashed) == FS_OK);
    CHECK(l->nodes[loaded_trashed].parent == l->downloads);
}

static void test_store_rejects_damage(void)
{
    struct fs *fs = fresh();
    int length = fs_store_save(fs, 7, g_image, sizeof g_image);
    uint32_t generation;
    int total;

    /* Any flipped bit is caught by a checksum. */
    for (int i = 0; i < length; i++) {
        if (i >= 28 && i < 32)
            continue; /* Reserved bytes. */
        memcpy(g_mutated, g_image, (size_t)length);
        g_mutated[i] ^= 0x10;
        CHECK(!fs_store_check(g_mutated, length, &generation, &total));
    }

    /* Truncated images are rejected. */
    for (int cut = 0; cut < length; cut += 7)
        CHECK(!fs_store_check(g_image, cut, &generation, &total));
    CHECK(!fs_store_save(fs, 1, g_image, 8));
}

/* Rewrites the checksums so that only the structure checks can reject an image. */
static void reseal(uint8_t *image, int length)
{
    uint32_t crc = fs_crc32(image + FS_STORE_HEADER, length - FS_STORE_HEADER);
    for (int i = 0; i < 4; i++)
        image[16 + i] = (uint8_t)(crc >> (8 * i));
    crc = fs_crc32(image, 24);
    for (int i = 0; i < 4; i++)
        image[24 + i] = (uint8_t)(crc >> (8 * i));
}

static void test_store_rejects_bad_structure(void)
{
    struct fs *fs = fresh();
    int length = fs_store_save(fs, 1, g_image, sizeof g_image);
    int first = FS_STORE_HEADER;

    /* A record whose parent comes after it could form a cycle. */
    memcpy(g_mutated, g_image, (size_t)length);
    g_mutated[first] = 200;
    reseal(g_mutated, length);
    CHECK(!fs_store_load(&g_loaded, g_mutated, length));

    /* An invalid node type. */
    memcpy(g_mutated, g_image, (size_t)length);
    g_mutated[first + 4] = 9;
    reseal(g_mutated, length);
    CHECK(!fs_store_load(&g_loaded, g_mutated, length));

    /* A name containing a path separator. */
    memcpy(g_mutated, g_image, (size_t)length);
    g_mutated[first + 14 + 1] = '/';
    reseal(g_mutated, length);
    CHECK(!fs_store_load(&g_loaded, g_mutated, length));

    /* A name length that runs past the end of the image. */
    memcpy(g_mutated, g_image, (size_t)length);
    g_mutated[first + 14] = 255;
    reseal(g_mutated, length);
    CHECK(!fs_store_load(&g_loaded, g_mutated, length));

    /* A node count larger than the table. */
    memcpy(g_mutated, g_image, (size_t)length);
    g_mutated[20] = 0xff;
    g_mutated[21] = 0xff;
    reseal(g_mutated, length);
    CHECK(!fs_store_load(&g_loaded, g_mutated, length));

    /* A standard folder is missing. */
    memcpy(g_mutated, g_image, (size_t)length);
    g_mutated[first + 5] = 0;
    reseal(g_mutated, length);
    CHECK(!fs_store_load(&g_loaded, g_mutated, length));
}

static void test_store_fuzz(void)
{
    struct fs *fs = fresh();
    int work = fs_create(fs, fs->documents, "Work", FS_DIR);
    int file = fs_create(fs, work, "a.txt", FS_FILE);
    fs_write(fs, file, "some file contents", 18);
    fs_trash(fs, file);
    int length = fs_store_save(fs, 3, g_image, sizeof g_image);

    uint32_t random = 0x2545f491u;
    for (int round = 0; round < 3000; round++) {
        memcpy(g_mutated, g_image, (size_t)length);
        int edits = 1 + round % 6;
        for (int e = 0; e < edits; e++) {
            random ^= random << 13;
            random ^= random >> 17;
            random ^= random << 5;
            int position = FS_STORE_HEADER + (int)(random % (uint32_t)(length - FS_STORE_HEADER));
            g_mutated[position] = (uint8_t)(random >> 24);
        }
        reseal(g_mutated, length);
        if (fs_store_load(&g_loaded, g_mutated, length)) {
            /* Anything accepted must be a usable filesystem. */
            int items[FS_MAX_NODES];
            CHECK(fs_is_dir(&g_loaded, g_loaded.trash));
            CHECK(fs_list(&g_loaded, FS_ROOT, items, FS_MAX_NODES) >= 5);
            CHECK(fs_create(&g_loaded, g_loaded.documents, "fuzz-check", FS_FILE) >= 0 ||
                  g_loaded.free_nodes == 0);
        }
    }
}

int main(void)
{
    test_crc32();
    test_revision_tracks_changes();
    test_store_round_trip();
    test_store_rejects_damage();
    test_store_rejects_bad_structure();
    test_store_fuzz();
    test_standard_layout();
    test_names();
    test_paths();
    test_read_write();
    test_out_of_space();
    test_node_exhaustion();
    test_create_and_rename();
    test_list_order();
    test_unique_names();
    test_move();
    test_copy_tree();
    test_delete_releases_everything();
    test_trash();
    test_trash_lost_origin();
    test_move_out_of_trash();
    test_empty_trash();
    test_time();

    if (failures) {
        printf("%d filesystem check(s) failed\n", failures);
        return 1;
    }
    puts("filesystem unit tests passed");
    return 0;
}
