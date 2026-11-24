#include "fs.h"

#include "text.h"

static int blocks_for(uint32_t size)
{
    return (int)((size + FS_BLOCK_SIZE - 1) / FS_BLOCK_SIZE);
}

static int is_live(const struct fs *fs, int node)
{
    return node >= 0 && node < FS_MAX_NODES && fs->nodes[node].type != FS_FREE;
}

int fs_is_dir(const struct fs *fs, int node)
{
    return is_live(fs, node) && fs->nodes[node].type == FS_DIR;
}

int fs_is_file(const struct fs *fs, int node)
{
    return is_live(fs, node) && fs->nodes[node].type == FS_FILE;
}

int fs_is_within(const struct fs *fs, int node, int ancestor)
{
    if (!is_live(fs, node) || !is_live(fs, ancestor))
        return 0;
    while (node != FS_NONE) {
        if (node == ancestor)
            return 1;
        node = fs->nodes[node].parent;
    }
    return 0;
}

uint32_t fs_make_time(int year, int month, int day, int hour, int minute)
{
    int y = year - 2000;
    if (y < 0)
        y = 0;
    if (y > 63)
        y = 63;
    return ((uint32_t)y << 26) | ((uint32_t)(month & 15) << 22) | ((uint32_t)(day & 31) << 17) |
           ((uint32_t)(hour & 31) << 12) | ((uint32_t)(minute & 63) << 6);
}

void fs_format_time(uint32_t time, char *out, int capacity)
{
    if (time == 0) {
        text_copy(out, capacity, "-");
        return;
    }
    out[0] = 0;
    text_append_u64(out, capacity, 2000 + (time >> 26));
    text_append(out, capacity, "-");
    text_append_2d(out, capacity, (time >> 22) & 15);
    text_append(out, capacity, "-");
    text_append_2d(out, capacity, (time >> 17) & 31);
    text_append(out, capacity, " ");
    text_append_2d(out, capacity, (time >> 12) & 31);
    text_append(out, capacity, ":");
    text_append_2d(out, capacity, (time >> 6) & 63);
}

const char *fs_error_text(int status)
{
    switch (status) {
    case FS_ERR_EXISTS:
        return "That name is already in use.";
    case FS_ERR_NOT_FOUND:
        return "Item not found.";
    case FS_ERR_NO_SPACE:
        return "Not enough memory for files.";
    case FS_ERR_BAD_NAME:
        return "That name is not valid.";
    case FS_ERR_NOT_DIR:
        return "That is not a folder.";
    case FS_ERR_IS_DIR:
        return "That is a folder.";
    case FS_ERR_PROTECTED:
        return "This item is protected.";
    case FS_ERR_CYCLE:
        return "A folder cannot go inside itself.";
    default:
        return "Unknown error.";
    }
}

int fs_valid_name(const char *name)
{
    int length = text_len(name);
    if (length == 0 || length >= FS_NAME_MAX)
        return 0;
    if (name[0] == ' ' || name[length - 1] == ' ')
        return 0;
    if (text_equal(name, ".") || text_equal(name, ".."))
        return 0;
    for (int i = 0; i < length; i++) {
        if (name[i] < 0x20 || name[i] > 0x7e || name[i] == '/' || name[i] == '"')
            return 0;
    }
    return 1;
}

int fs_lookup(const struct fs *fs, int dir, const char *name)
{
    if (!fs_is_dir(fs, dir))
        return FS_ERR_NOT_DIR;
    for (int child = fs->nodes[dir].first_child; child != FS_NONE;
         child = fs->nodes[child].next_sibling) {
        if (text_equal(fs->nodes[child].name, name))
            return child;
    }
    return FS_ERR_NOT_FOUND;
}

int fs_resolve(const struct fs *fs, int cwd, const char *path)
{
    int node = path[0] == '/' ? FS_ROOT : cwd;
    if (!is_live(fs, node))
        return FS_ERR_NOT_FOUND;

    while (*path) {
        while (*path == '/')
            path++;
        if (!*path)
            break;

        char part[FS_NAME_MAX];
        int length = 0;
        while (*path && *path != '/') {
            if (length >= FS_NAME_MAX - 1)
                return FS_ERR_NOT_FOUND;
            part[length++] = *path++;
        }
        part[length] = 0;

        if (!fs_is_dir(fs, node))
            return FS_ERR_NOT_DIR;
        if (text_equal(part, "."))
            continue;
        if (text_equal(part, "..")) {
            if (node != FS_ROOT)
                node = fs->nodes[node].parent;
            continue;
        }
        node = fs_lookup(fs, node, part);
        if (node < 0)
            return node;
    }
    return node;
}

void fs_path(const struct fs *fs, int node, char *out, int capacity)
{
    int chain[FS_MAX_NODES];
    int depth = 0;

    text_copy(out, capacity, "/");
    if (!is_live(fs, node))
        return;
    while (node != FS_ROOT && node != FS_NONE && depth < FS_MAX_NODES) {
        chain[depth++] = node;
        node = fs->nodes[node].parent;
    }
    out[0] = 0;
    if (depth == 0)
        text_copy(out, capacity, "/");
    for (int i = depth - 1; i >= 0; i--) {
        text_append(out, capacity, "/");
        text_append(out, capacity, fs->nodes[chain[i]].name);
    }
}

static char lower(char c)
{
    return c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c;
}

static int sorts_before(const struct fs *fs, int a, int b)
{
    const struct fs_node *x = &fs->nodes[a];
    const struct fs_node *y = &fs->nodes[b];
    if (x->type != y->type)
        return x->type == FS_DIR;

    for (int i = 0; i < FS_NAME_MAX; i++) {
        char p = lower(x->name[i]);
        char q = lower(y->name[i]);
        if (p != q)
            return p < q;
        if (!p)
            break;
    }
    return 0;
}

int fs_list(const struct fs *fs, int dir, int *out, int max)
{
    if (!fs_is_dir(fs, dir))
        return 0;

    int count = 0;
    for (int child = fs->nodes[dir].first_child; child != FS_NONE && count < max;
         child = fs->nodes[child].next_sibling) {
        int position = count;
        while (position > 0 && sorts_before(fs, child, out[position - 1])) {
            out[position] = out[position - 1];
            position--;
        }
        out[position] = child;
        count++;
    }
    return count;
}

void fs_unique_name(const struct fs *fs, int dir, const char *name, char *out, int capacity)
{
    text_copy(out, capacity, name);
    if (fs_lookup(fs, dir, out) < 0)
        return;

    int length = text_len(name);
    int dot = length;
    for (int i = 1; i < length; i++) {
        if (name[i] == '.')
            dot = i;
    }
    if (length - dot > 8)
        dot = length; /* Treat a very long suffix as part of the name. */

    for (int n = 2; n < 1000; n++) {
        char suffix[8] = " (";
        text_append_u64(suffix, sizeof suffix, (uint64_t)n);
        text_append(suffix, sizeof suffix, ")");

        int keep = capacity - 1 - text_len(suffix) - (length - dot);
        if (keep > dot)
            keep = dot;
        if (keep < 1)
            keep = 1;

        char candidate[FS_NAME_MAX];
        int position = 0;
        for (int i = 0; i < keep && position < FS_NAME_MAX - 1; i++)
            candidate[position++] = name[i];
        candidate[position] = 0;
        text_append(candidate, sizeof candidate, suffix);
        text_append(candidate, sizeof candidate, name + dot);

        if (fs_lookup(fs, dir, candidate) < 0) {
            text_copy(out, capacity, candidate);
            return;
        }
    }
}

static void link_child(struct fs *fs, int parent, int node)
{
    fs->nodes[node].parent = (int16_t)parent;
    fs->nodes[node].next_sibling = fs->nodes[parent].first_child;
    fs->nodes[parent].first_child = (int16_t)node;
}

static void unlink_child(struct fs *fs, int node)
{
    int16_t *link = &fs->nodes[fs->nodes[node].parent].first_child;
    while (*link != node)
        link = &fs->nodes[*link].next_sibling;
    *link = fs->nodes[node].next_sibling;
    fs->nodes[node].next_sibling = FS_NONE;
}

static int alloc_node(struct fs *fs, int parent, const char *name, int type)
{
    for (int i = 1; i < FS_MAX_NODES; i++) {
        struct fs_node *n = &fs->nodes[i];
        if (n->type != FS_FREE)
            continue;

        n->type = (uint8_t)type;
        n->is_protected = 0;
        n->first_child = FS_NONE;
        n->next_sibling = FS_NONE;
        n->trash_origin = FS_NONE;
        n->first_block = FS_NONE;
        n->size = 0;
        n->modified = fs->now;
        n->trash_name[0] = 0;
        text_copy(n->name, FS_NAME_MAX, name);
        link_child(fs, parent, i);
        fs->free_nodes--;
        fs->revision++;
        return i;
    }
    return FS_ERR_NO_SPACE;
}

int fs_add_node(struct fs *fs, int parent, const char *name, int type)
{
    return alloc_node(fs, parent, name, type);
}

static void free_chain(struct fs *fs, int32_t block)
{
    while (block != FS_NONE) {
        int32_t next = fs->block_next[block];
        fs->block_next[block] = fs->free_block;
        fs->free_block = block;
        fs->free_blocks++;
        block = next;
    }
}

/* The caller must already have checked that enough blocks are free. */
static int32_t alloc_chain(struct fs *fs, int count)
{
    int32_t head = FS_NONE;
    for (int i = 0; i < count; i++) {
        int32_t block = fs->free_block;
        fs->free_block = fs->block_next[block];
        fs->free_blocks--;
        fs->block_next[block] = head;
        head = block;
    }
    return head;
}

int fs_create(struct fs *fs, int parent, const char *name, int type)
{
    if (!fs_is_dir(fs, parent))
        return FS_ERR_NOT_DIR;
    if (fs_is_within(fs, parent, fs->trash))
        return FS_ERR_PROTECTED;
    if (!fs_valid_name(name))
        return FS_ERR_BAD_NAME;
    if (fs_lookup(fs, parent, name) >= 0)
        return FS_ERR_EXISTS;
    return alloc_node(fs, parent, name, type);
}

int fs_write(struct fs *fs, int node, const char *data, int length)
{
    if (!is_live(fs, node))
        return FS_ERR_NOT_FOUND;
    if (!fs_is_file(fs, node))
        return FS_ERR_IS_DIR;
    if (length < 0)
        return FS_ERR_NO_SPACE;

    struct fs_node *n = &fs->nodes[node];
    int needed = blocks_for((uint32_t)length);
    if (needed > fs->free_blocks + blocks_for(n->size))
        return FS_ERR_NO_SPACE;

    free_chain(fs, n->first_block);
    n->first_block = alloc_chain(fs, needed);
    n->size = (uint32_t)length;
    n->modified = fs->now;
    fs->revision++;

    int written = 0;
    for (int32_t block = n->first_block; block != FS_NONE; block = fs->block_next[block]) {
        for (int i = 0; i < FS_BLOCK_SIZE && written < length; i++)
            fs->blocks[block][i] = (uint8_t)data[written++];
    }
    return FS_OK;
}

int fs_read(const struct fs *fs, int node, int offset, char *out, int length)
{
    if (!is_live(fs, node))
        return FS_ERR_NOT_FOUND;
    if (!fs_is_file(fs, node))
        return FS_ERR_IS_DIR;

    const struct fs_node *n = &fs->nodes[node];
    if (offset < 0 || length <= 0 || (uint32_t)offset >= n->size)
        return 0;
    if ((uint32_t)length > n->size - (uint32_t)offset)
        length = (int)(n->size - (uint32_t)offset);

    int32_t block = n->first_block;
    for (int skip = offset / FS_BLOCK_SIZE; skip > 0; skip--)
        block = fs->block_next[block];

    int position = offset % FS_BLOCK_SIZE;
    for (int copied = 0; copied < length; copied++) {
        if (position == FS_BLOCK_SIZE) {
            block = fs->block_next[block];
            position = 0;
        }
        out[copied] = (char)fs->blocks[block][position++];
    }
    return length;
}

int fs_rename(struct fs *fs, int node, const char *name)
{
    if (!is_live(fs, node))
        return FS_ERR_NOT_FOUND;
    if (fs->nodes[node].is_protected || fs_is_within(fs, node, fs->trash))
        return FS_ERR_PROTECTED;
    if (!fs_valid_name(name))
        return FS_ERR_BAD_NAME;

    int existing = fs_lookup(fs, fs->nodes[node].parent, name);
    if (existing == node)
        return FS_OK;
    if (existing >= 0)
        return FS_ERR_EXISTS;
    text_copy(fs->nodes[node].name, FS_NAME_MAX, name);
    fs->nodes[node].modified = fs->now;
    fs->revision++;
    return FS_OK;
}

static void relocate(struct fs *fs, int node, int dir, const char *source_name)
{
    char unique[FS_NAME_MAX];
    fs_unique_name(fs, dir, source_name, unique, sizeof unique);
    unlink_child(fs, node);
    text_copy(fs->nodes[node].name, FS_NAME_MAX, unique);
    link_child(fs, dir, node);
    fs->revision++;
}

int fs_move(struct fs *fs, int node, int dir)
{
    if (!is_live(fs, node))
        return FS_ERR_NOT_FOUND;
    if (fs->nodes[node].is_protected)
        return FS_ERR_PROTECTED;
    if (!fs_is_dir(fs, dir))
        return FS_ERR_NOT_DIR;
    if (fs_is_within(fs, dir, node))
        return FS_ERR_CYCLE;
    if (fs->nodes[node].parent == dir)
        return FS_OK;
    if (dir == fs->trash)
        return fs_trash(fs, node);
    if (fs_is_within(fs, dir, fs->trash))
        return FS_ERR_PROTECTED;

    char name[FS_NAME_MAX];
    text_copy(name, sizeof name, fs->nodes[node].name);
    if (fs->nodes[node].parent == fs->trash && fs->nodes[node].trash_name[0])
        text_copy(name, sizeof name, fs->nodes[node].trash_name);
    relocate(fs, node, dir, name);
    fs->nodes[node].trash_origin = FS_NONE;
    fs->nodes[node].trash_name[0] = 0;
    return FS_OK;
}

static void count_tree(const struct fs *fs, int node, int *nodes, int *blocks)
{
    (*nodes)++;
    *blocks += blocks_for(fs->nodes[node].size);
    for (int child = fs->nodes[node].first_child; child != FS_NONE;
         child = fs->nodes[child].next_sibling)
        count_tree(fs, child, nodes, blocks);
}

static int copy_tree(struct fs *fs, int node, int parent, const char *name)
{
    int copy = alloc_node(fs, parent, name, fs->nodes[node].type);
    fs->nodes[copy].modified = fs->nodes[node].modified;

    if (fs->nodes[node].type == FS_FILE) {
        int count = blocks_for(fs->nodes[node].size);
        fs->nodes[copy].first_block = alloc_chain(fs, count);
        fs->nodes[copy].size = fs->nodes[node].size;

        int32_t from = fs->nodes[node].first_block;
        int32_t to = fs->nodes[copy].first_block;
        while (from != FS_NONE) {
            for (int i = 0; i < FS_BLOCK_SIZE; i++)
                fs->blocks[to][i] = fs->blocks[from][i];
            from = fs->block_next[from];
            to = fs->block_next[to];
        }
    }

    for (int child = fs->nodes[node].first_child; child != FS_NONE;
         child = fs->nodes[child].next_sibling)
        copy_tree(fs, child, copy, fs->nodes[child].name);
    return copy;
}

int fs_copy(struct fs *fs, int node, int dir)
{
    if (!is_live(fs, node))
        return FS_ERR_NOT_FOUND;
    if (node == FS_ROOT || fs->nodes[node].is_protected)
        return FS_ERR_PROTECTED;
    if (!fs_is_dir(fs, dir))
        return FS_ERR_NOT_DIR;
    if (fs_is_within(fs, dir, node))
        return FS_ERR_CYCLE;
    if (fs_is_within(fs, dir, fs->trash))
        return FS_ERR_PROTECTED;

    /* Check capacity first so a failed copy leaves nothing behind. */
    int nodes = 0, blocks = 0;
    count_tree(fs, node, &nodes, &blocks);
    if (nodes > fs->free_nodes || blocks > fs->free_blocks)
        return FS_ERR_NO_SPACE;

    char name[FS_NAME_MAX];
    fs_unique_name(fs, dir, fs->nodes[node].name, name, sizeof name);
    return copy_tree(fs, node, dir, name);
}

static void delete_tree(struct fs *fs, int node)
{
    while (fs->nodes[node].first_child != FS_NONE)
        delete_tree(fs, fs->nodes[node].first_child);

    unlink_child(fs, node);
    free_chain(fs, fs->nodes[node].first_block);
    fs->nodes[node].first_block = FS_NONE;
    fs->nodes[node].type = FS_FREE;
    fs->free_nodes++;
    fs->revision++;

    /* Trashed items from this folder can no longer return to it. */
    for (int i = 0; i < FS_MAX_NODES; i++) {
        if (fs->nodes[i].type != FS_FREE && fs->nodes[i].trash_origin == node)
            fs->nodes[i].trash_origin = FS_NONE;
    }
}

int fs_delete(struct fs *fs, int node)
{
    if (!is_live(fs, node))
        return FS_ERR_NOT_FOUND;
    if (node == FS_ROOT || fs->nodes[node].is_protected)
        return FS_ERR_PROTECTED;
    delete_tree(fs, node);
    return FS_OK;
}

int fs_trash(struct fs *fs, int node)
{
    if (!is_live(fs, node))
        return FS_ERR_NOT_FOUND;
    if (node == FS_ROOT || fs->nodes[node].is_protected || fs_is_within(fs, node, fs->trash))
        return FS_ERR_PROTECTED;

    struct fs_node *n = &fs->nodes[node];
    char original[FS_NAME_MAX];
    text_copy(original, sizeof original, n->name);
    int origin = n->parent;

    relocate(fs, node, fs->trash, original);
    text_copy(n->trash_name, FS_NAME_MAX, original);
    n->trash_origin = (int16_t)origin;
    return FS_OK;
}

int fs_restore(struct fs *fs, int node)
{
    if (!is_live(fs, node) || fs->nodes[node].parent != fs->trash)
        return FS_ERR_NOT_FOUND;

    struct fs_node *n = &fs->nodes[node];
    int destination = n->trash_origin;
    if (!fs_is_dir(fs, destination) || fs_is_within(fs, destination, fs->trash))
        destination = fs->documents;

    char original[FS_NAME_MAX];
    text_copy(original, sizeof original, n->trash_name[0] ? n->trash_name : n->name);
    relocate(fs, node, destination, original);
    n->trash_origin = FS_NONE;
    n->trash_name[0] = 0;
    return FS_OK;
}

int fs_empty_trash(struct fs *fs)
{
    while (fs->nodes[fs->trash].first_child != FS_NONE)
        delete_tree(fs, fs->nodes[fs->trash].first_child);
    return FS_OK;
}

uint32_t fs_used_bytes(const struct fs *fs)
{
    return (uint32_t)(FS_BLOCKS - fs->free_blocks) * FS_BLOCK_SIZE;
}

static int standard_folder(struct fs *fs, const char *name)
{
    int node = alloc_node(fs, FS_ROOT, name, FS_DIR);
    fs->nodes[node].is_protected = 1;
    return node;
}

static void starter_file(struct fs *fs, int dir, const char *name, const char *text)
{
    int node = alloc_node(fs, dir, name, FS_FILE);
    if (node >= 0)
        fs_write(fs, node, text, text_len(text));
}

void fs_format(struct fs *fs)
{
    for (int i = 0; i < FS_MAX_NODES; i++) {
        fs->nodes[i].type = FS_FREE;
        fs->nodes[i].first_block = FS_NONE;
        fs->nodes[i].trash_origin = FS_NONE;
    }
    for (int i = 0; i < FS_BLOCKS; i++)
        fs->block_next[i] = i + 1 < FS_BLOCKS ? i + 1 : FS_NONE;
    fs->free_block = 0;
    fs->free_blocks = FS_BLOCKS;
    fs->free_nodes = FS_MAX_NODES - 1;
    fs->now = 0;
    fs->revision = 0;
    fs->desktop = FS_NONE;
    fs->documents = FS_NONE;
    fs->downloads = FS_NONE;
    fs->pictures = FS_NONE;
    fs->trash = FS_NONE;

    struct fs_node *root = &fs->nodes[FS_ROOT];
    root->type = FS_DIR;
    root->is_protected = 1;
    root->parent = FS_NONE;
    root->first_child = FS_NONE;
    root->next_sibling = FS_NONE;
    root->size = 0;
    root->modified = 0;
    root->name[0] = 0;
    root->trash_name[0] = 0;
}

void fs_init(struct fs *fs)
{
    fs_format(fs);
    fs->desktop = standard_folder(fs, "Desktop");
    fs->documents = standard_folder(fs, "Documents");
    fs->downloads = standard_folder(fs, "Downloads");
    fs->pictures = standard_folder(fs, "Pictures");
    fs->trash = standard_folder(fs, "Trash");

    starter_file(fs, fs->desktop, "Welcome.txt",
                 "Welcome to Cato OS!\n\n"
                 "Double-click icons to open them.\n"
                 "Right-click for more actions.\n\n"
                 "When the Cato USB drive is present,\n"
                 "your files are saved to it. Without\n"
                 "it, they last only until you exit.\n");
    starter_file(fs, fs->documents, "Notes.txt", "My notes\n");
}
