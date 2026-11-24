#include "fs_store.h"

#include "text.h"

#define NO_ORIGIN 0xffffu
#define RECORD_FIXED 14

static const char MAGIC[8] = { 'C', 'A', 'T', 'O', 'F', 'S', '0', '1' };

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t get16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static void put32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

static void put16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
}

uint32_t fs_crc32(const uint8_t *data, int length)
{
    static uint32_t table[256];
    static int ready;

    if (!ready) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t value = i;
            for (int bit = 0; bit < 8; bit++)
                value = (value & 1) ? 0xedb88320u ^ (value >> 1) : value >> 1;
            table[i] = value;
        }
        ready = 1;
    }

    uint32_t crc = 0xffffffffu;
    for (int i = 0; i < length; i++)
        crc = table[(crc ^ data[i]) & 0xff] ^ (crc >> 8);
    return ~crc;
}

/* Lists nodes parent-first, so a loader always sees a folder before its contents. */
static void collect(const struct fs *fs, int node, int *order, int *index_of, int *count)
{
    index_of[node] = *count;
    order[(*count)++] = node;
    for (int child = fs->nodes[node].first_child; child != FS_NONE;
         child = fs->nodes[child].next_sibling)
        collect(fs, child, order, index_of, count);
}

int fs_store_save(const struct fs *fs, uint32_t generation, uint8_t *out, int capacity)
{
    int order[FS_MAX_NODES];
    int index_of[FS_MAX_NODES];
    int count = 0;

    if (capacity < FS_STORE_HEADER)
        return 0;
    for (int i = 0; i < FS_MAX_NODES; i++)
        index_of[i] = -1;
    collect(fs, FS_ROOT, order, index_of, &count);

    int p = FS_STORE_HEADER;
    for (int i = 1; i < count; i++) {
        const struct fs_node *n = &fs->nodes[order[i]];
        int name_length = text_len(n->name);
        int trash_length = text_len(n->trash_name);
        int size = (int)n->size;
        if (p + RECORD_FIXED + 2 + name_length + trash_length + size > capacity)
            return 0;

        int origin = n->trash_origin >= 0 ? index_of[n->trash_origin] : -1;
        put16(out + p, (uint16_t)index_of[n->parent]);
        put16(out + p + 2, origin >= 0 ? (uint16_t)origin : (uint16_t)NO_ORIGIN);
        out[p + 4] = n->type;
        out[p + 5] = n->is_protected;
        put32(out + p + 6, n->modified);
        put32(out + p + 10, n->size);
        p += RECORD_FIXED;

        out[p++] = (uint8_t)name_length;
        for (int k = 0; k < name_length; k++)
            out[p++] = (uint8_t)n->name[k];
        out[p++] = (uint8_t)trash_length;
        for (int k = 0; k < trash_length; k++)
            out[p++] = (uint8_t)n->trash_name[k];

        if (size > 0) {
            if (fs_read(fs, order[i], 0, (char *)out + p, size) != size)
                return 0;
            p += size;
        }
    }

    for (int i = 0; i < 8; i++)
        out[i] = (uint8_t)MAGIC[i];
    put32(out + 8, generation);
    put32(out + 12, (uint32_t)(p - FS_STORE_HEADER));
    put32(out + 16, fs_crc32(out + FS_STORE_HEADER, p - FS_STORE_HEADER));
    put32(out + 20, (uint32_t)(count - 1));
    put32(out + 24, fs_crc32(out, 24));
    put32(out + 28, 0);
    return p;
}

int fs_store_check(const uint8_t *data, int length, uint32_t *generation, int *total)
{
    if (length < FS_STORE_HEADER)
        return 0;
    for (int i = 0; i < 8; i++) {
        if (data[i] != (uint8_t)MAGIC[i])
            return 0;
    }
    if (get32(data + 24) != fs_crc32(data, 24))
        return 0;

    uint32_t payload = get32(data + 12);
    if (payload > (uint32_t)(length - FS_STORE_HEADER) ||
        payload > (uint32_t)(FS_STORE_MAX - FS_STORE_HEADER))
        return 0;
    if (get32(data + 16) != fs_crc32(data + FS_STORE_HEADER, (int)payload))
        return 0;

    *generation = get32(data + 8);
    *total = FS_STORE_HEADER + (int)payload;
    return 1;
}

static int is_standard_name(const char *name)
{
    return text_equal(name, "Desktop") || text_equal(name, "Documents") ||
           text_equal(name, "Downloads") || text_equal(name, "Pictures") ||
           text_equal(name, "Trash");
}

static int standard_folder(const struct fs *fs, const char *name)
{
    int node = fs_lookup(fs, FS_ROOT, name);
    return fs_is_dir(fs, node) && fs->nodes[node].is_protected ? node : FS_NONE;
}

/* Reads a length-prefixed string. Returns the new offset, or -1 if invalid. */
static int read_name(const uint8_t *data, int p, int end, char *out)
{
    if (p >= end)
        return -1;
    int length = data[p++];
    if (length >= FS_NAME_MAX || length > end - p)
        return -1;
    for (int i = 0; i < length; i++)
        out[i] = (char)data[p + i];
    out[length] = 0;
    return p + length;
}

int fs_store_load(struct fs *fs, const uint8_t *data, int length)
{
    uint32_t generation;
    int total;
    if (!fs_store_check(data, length, &generation, &total))
        return 0;

    uint32_t node_count = get32(data + 20);
    if (node_count >= FS_MAX_NODES)
        return 0;

    int map[FS_MAX_NODES];
    int origins[FS_MAX_NODES];
    fs_format(fs);
    map[0] = FS_ROOT;
    origins[0] = -1;

    int p = FS_STORE_HEADER;
    for (int index = 1; index <= (int)node_count; index++) {
        if (total - p < RECORD_FIXED)
            return 0;

        int parent_index = get16(data + p);
        int origin_index = get16(data + p + 2);
        int type = data[p + 4];
        int is_protected = data[p + 5];
        uint32_t modified = get32(data + p + 6);
        uint32_t size = get32(data + p + 10);
        p += RECORD_FIXED;

        /* A parent must come first, which also makes cycles impossible. */
        if (parent_index >= index || (type != FS_DIR && type != FS_FILE) || is_protected > 1)
            return 0;
        int parent = map[parent_index];
        if (!fs_is_dir(fs, parent))
            return 0;

        char name[FS_NAME_MAX];
        char trash_name[FS_NAME_MAX];
        p = read_name(data, p, total, name);
        if (p < 0)
            return 0;
        p = read_name(data, p, total, trash_name);
        if (p < 0)
            return 0;
        if (!fs_valid_name(name) || (trash_name[0] && !fs_valid_name(trash_name)))
            return 0;
        if (fs_lookup(fs, parent, name) >= 0)
            return 0;
        if (is_protected && (parent != FS_ROOT || type != FS_DIR || !is_standard_name(name)))
            return 0;
        if (type == FS_DIR ? size != 0
                           : size > (uint32_t)(total - p) || size > FS_BLOCKS * FS_BLOCK_SIZE)
            return 0;

        int node = fs_add_node(fs, parent, name, type);
        if (node < 0)
            return 0;
        if (type == FS_FILE) {
            if (fs_write(fs, node, (const char *)data + p, (int)size) != FS_OK)
                return 0;
            p += (int)size;
        }
        fs->nodes[node].is_protected = (uint8_t)is_protected;
        fs->nodes[node].modified = modified;
        text_copy(fs->nodes[node].trash_name, FS_NAME_MAX, trash_name);
        map[index] = node;
        origins[index] = origin_index == NO_ORIGIN ? -1 : origin_index;
    }
    if (p != total)
        return 0;

    for (int index = 1; index <= (int)node_count; index++) {
        if (origins[index] < 0)
            continue;
        if (origins[index] > (int)node_count || !fs_is_dir(fs, map[origins[index]]))
            return 0;
        fs->nodes[map[index]].trash_origin = (int16_t)map[origins[index]];
    }

    fs->desktop = standard_folder(fs, "Desktop");
    fs->documents = standard_folder(fs, "Documents");
    fs->downloads = standard_folder(fs, "Downloads");
    fs->pictures = standard_folder(fs, "Pictures");
    fs->trash = standard_folder(fs, "Trash");
    if (fs->desktop == FS_NONE || fs->documents == FS_NONE || fs->downloads == FS_NONE ||
        fs->pictures == FS_NONE || fs->trash == FS_NONE)
        return 0;

    fs->revision = 0;
    return 1;
}
