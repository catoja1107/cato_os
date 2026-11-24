/* The Terminal app and its built-in commands. */
#include "apps.h"
#include "net.h"
#include "shell.h"
#include "text.h"

#define PADDING 8
#define MAX_ARGS 4
#define CAT_LIMIT 1024

enum ping_phase { PING_PHASE_NONE, PING_PHASE_NETWORK, PING_PHASE_DNS, PING_PHASE_REPLY };

void terminal_draw(struct wm *wm, struct rect content, int focused)
{
    const struct terminal *t = &wm->terminal;
    int x = content.x + PADDING;
    int y = content.y + PADDING;

    for (int row = 0; row < TERM_ROWS - 1; row++, y += FONT_LINE)
        canvas_text(wm->screen, x, y, t->lines[row], UI_TEXT);

    canvas_text(wm->screen, x, y, ">", UI_ACCENT);
    canvas_text(wm->screen, x + 2 * FONT_ADVANCE, y, t->input, UI_TEXT);

    struct rect cursor = { x + (2 + t->input_len) * FONT_ADVANCE, y, FONT_ADVANCE - 2, FONT_HEIGHT };
    if (focused)
        canvas_fill(wm->screen, cursor, UI_ACCENT);
    else
        canvas_frame(wm->screen, cursor, UI_DIM);
}

static void terminal_push(struct terminal *t, const char *text, int length)
{
    for (int row = 0; row < TERM_ROWS - 2; row++)
        text_copy(t->lines[row], TERM_COLS + 1, t->lines[row + 1]);

    char *last = t->lines[TERM_ROWS - 2];
    int i = 0;
    for (; i < length && i < TERM_COLS; i++)
        last[i] = text[i];
    last[i] = 0;
}

void terminal_print(struct terminal *t, const char *text)
{
    int length = text_len(text);
    if (length == 0) {
        terminal_push(t, "", 0);
        return;
    }
    while (length > 0) {
        int chunk = length < TERM_COLS ? length : TERM_COLS;
        terminal_push(t, text, chunk);
        text += chunk;
        length -= chunk;
    }
}

static void terminal_clear(struct terminal *t)
{
    for (int row = 0; row < TERM_ROWS - 1; row++)
        t->lines[row][0] = 0;
}

void terminal_init(struct terminal *t)
{
    terminal_clear(t);
    t->input[0] = 0;
    t->input_len = 0;
    t->cwd = FS_ROOT;
    t->ping_phase = PING_PHASE_NONE;
    t->ping_host[0] = 0;
    terminal_print(t, "Cato OS terminal");
    terminal_print(t, "Type help for a list of commands.");
    terminal_print(t, "");
}

void terminal_forget(struct wm *wm, int node)
{
    if (fs_is_within(wm->fs, wm->terminal.cwd, node))
        wm->terminal.cwd = wm->fs->nodes[node].parent;
}

void terminal_sync(struct wm *wm)
{
    if (!fs_is_dir(wm->fs, wm->terminal.cwd))
        wm->terminal.cwd = FS_ROOT;
}

static void print_field(struct terminal *t, const char *label, const char *value)
{
    char line[TERM_COLS * 2];
    text_copy(line, sizeof line, label);
    text_append(line, sizeof line, value);
    terminal_print(t, line);
}

static void print_error(struct terminal *t, const char *command, int status)
{
    char line[TERM_COLS * 2];
    text_copy(line, sizeof line, command);
    text_append(line, sizeof line, ": ");
    text_append(line, sizeof line, fs_error_text(status));
    terminal_print(t, line);
}

/* Splits on spaces and honors double quotes. Returns -1 if there are too many arguments. */
static int parse_args(char *line, char **argv)
{
    int argc = 0;
    char *p = line;

    while (*p) {
        while (*p == ' ')
            p++;
        if (!*p)
            break;
        if (argc == MAX_ARGS)
            return -1;

        char *out = p;
        int quoted = 0;
        argv[argc++] = out;
        while (*p && (quoted || *p != ' ')) {
            if (*p == '"') {
                quoted = !quoted;
                p++;
                continue;
            }
            *out++ = *p++;
        }
        if (*p)
            p++;
        *out = 0;
    }
    return argc;
}

static int split_parent(const struct wm *wm, const char *path, int *parent, char *name)
{
    int slash = -1;
    int length = text_len(path);
    for (int i = 0; i < length; i++) {
        if (path[i] == '/')
            slash = i;
    }

    if (slash < 0) {
        *parent = wm->terminal.cwd;
    } else {
        char dir[TERM_COLS];
        int dir_length = slash == 0 ? 1 : slash;
        if (dir_length >= (int)sizeof dir)
            return FS_ERR_NOT_FOUND;
        for (int i = 0; i < dir_length; i++)
            dir[i] = path[i];
        dir[dir_length] = 0;

        *parent = fs_resolve(wm->fs, wm->terminal.cwd, dir);
        if (*parent < 0)
            return *parent;
        if (!fs_is_dir(wm->fs, *parent))
            return FS_ERR_NOT_DIR;
    }

    const char *leaf = path + slash + 1;
    if (text_len(leaf) >= FS_NAME_MAX)
        return FS_ERR_BAD_NAME;
    text_copy(name, FS_NAME_MAX, leaf);
    return FS_OK;
}

static void command_help(struct terminal *t)
{
    terminal_print(t, "Files:  ls cd pwd mkdir touch cat edit");
    terminal_print(t, "        cp mv rm (rm moves to Trash)");
    terminal_print(t, "Apps:   files editor system browser");
    terminal_print(t, "Net:    net  ping HOST  browser URL");
    terminal_print(t, "Info:   about sysinfo time");
    terminal_print(t, "Other:  echo clear exit");
    terminal_print(t, "Use quotes for names with spaces.");
}

static void command_ls(struct wm *wm, int argc, char **argv)
{
    struct terminal *t = &wm->terminal;
    int dir = argc > 1 ? fs_resolve(wm->fs, t->cwd, argv[1]) : t->cwd;
    if (dir < 0) {
        print_error(t, "ls", dir);
        return;
    }
    if (!fs_is_dir(wm->fs, dir)) {
        terminal_print(t, wm->fs->nodes[dir].name);
        return;
    }

    int items[FS_MAX_NODES];
    int count = fs_list(wm->fs, dir, items, FS_MAX_NODES);
    if (count == 0) {
        terminal_print(t, "(empty)");
        return;
    }

    char row[TERM_COLS + 1] = "";
    for (int i = 0; i < count; i++) {
        char entry[FS_NAME_MAX + 3];
        text_copy(entry, sizeof entry, wm->fs->nodes[items[i]].name);
        if (fs_is_dir(wm->fs, items[i]))
            text_append(entry, sizeof entry, "/");
        if (row[0] && text_len(row) + text_len(entry) > TERM_COLS) {
            terminal_print(t, row);
            row[0] = 0;
        }
        text_append(row, sizeof row, entry);
        text_append(row, sizeof row, "  ");
    }
    if (row[0])
        terminal_print(t, row);
}

static void command_cd(struct wm *wm, int argc, char **argv)
{
    struct terminal *t = &wm->terminal;
    int dir = argc > 1 ? fs_resolve(wm->fs, t->cwd, argv[1]) : FS_ROOT;
    if (dir < 0)
        print_error(t, "cd", dir);
    else if (!fs_is_dir(wm->fs, dir))
        print_error(t, "cd", FS_ERR_NOT_DIR);
    else
        t->cwd = dir;
}

static void command_create(struct wm *wm, const char *command, const char *path, int type)
{
    struct terminal *t = &wm->terminal;
    char name[FS_NAME_MAX];
    int parent;
    int status = split_parent(wm, path, &parent, name);
    if (status == FS_OK) {
        /* touch on an existing file is not an error. */
        if (type == FS_FILE && fs_is_file(wm->fs, fs_lookup(wm->fs, parent, name)))
            return;
        status = fs_create(wm->fs, parent, name, type);
    }
    if (status < 0)
        print_error(t, command, status);
    else
        shell_fs_changed(wm);
}

static void command_cat(struct wm *wm, const char *path)
{
    struct terminal *t = &wm->terminal;
    int node = fs_resolve(wm->fs, t->cwd, path);
    if (node < 0) {
        print_error(t, "cat", node);
        return;
    }
    if (!fs_is_file(wm->fs, node)) {
        print_error(t, "cat", FS_ERR_IS_DIR);
        return;
    }

    char buffer[CAT_LIMIT + 1];
    int length = fs_read(wm->fs, node, 0, buffer, CAT_LIMIT);
    int start = 0;
    for (int i = 0; i <= length; i++) {
        if (i == length || buffer[i] == '\n') {
            if (i == length && i == start)
                break;
            char saved = buffer[i];
            buffer[i] = 0;
            terminal_print(t, buffer + start);
            buffer[i] = saved;
            start = i + 1;
        }
    }
    if ((uint32_t)length < wm->fs->nodes[node].size)
        terminal_print(t, "(output truncated)");
}

static void command_rm(struct wm *wm, const char *path)
{
    struct terminal *t = &wm->terminal;
    int node = fs_resolve(wm->fs, t->cwd, path);
    int status = node < 0 ? node : fs_trash(wm->fs, node);
    if (status < 0)
        print_error(t, "rm", status);
    else
        shell_fs_changed(wm);
}

/* Finds where cp or mv should put an item. name is empty when keeping the source name. */
static int destination(const struct wm *wm, const char *path, int *dir, char *name)
{
    int target = fs_resolve(wm->fs, wm->terminal.cwd, path);
    if (target >= 0 && fs_is_dir(wm->fs, target)) {
        *dir = target;
        name[0] = 0;
        return FS_OK;
    }
    if (target >= 0)
        return FS_ERR_EXISTS;

    int status = split_parent(wm, path, dir, name);
    if (status < 0)
        return status;
    return fs_valid_name(name) ? FS_OK : FS_ERR_BAD_NAME;
}

static void command_cp(struct wm *wm, const char *from, const char *to)
{
    struct terminal *t = &wm->terminal;
    char name[FS_NAME_MAX];
    int dir;
    int source = fs_resolve(wm->fs, t->cwd, from);
    int status = source < 0 ? source : destination(wm, to, &dir, name);

    if (status >= 0) {
        int copy = fs_copy(wm->fs, source, dir);
        status = copy < 0 ? copy : FS_OK;
        if (copy >= 0 && name[0]) {
            status = fs_rename(wm->fs, copy, name);
            if (status < 0)
                fs_delete(wm->fs, copy);
        }
    }
    if (status < 0)
        print_error(t, "cp", status);
    else
        shell_fs_changed(wm);
}

static void command_mv(struct wm *wm, const char *from, const char *to)
{
    struct terminal *t = &wm->terminal;
    char name[FS_NAME_MAX];
    int dir;
    int source = fs_resolve(wm->fs, t->cwd, from);
    int status = source < 0 ? source : destination(wm, to, &dir, name);

    if (status >= 0) {
        char original[FS_NAME_MAX];
        int old_parent = wm->fs->nodes[source].parent;
        text_copy(original, sizeof original, wm->fs->nodes[source].name);

        status = fs_move(wm->fs, source, dir);
        if (status >= 0 && name[0]) {
            status = fs_rename(wm->fs, source, name);
            if (status < 0 && old_parent != dir) {
                fs_move(wm->fs, source, old_parent);
                fs_rename(wm->fs, source, original);
            }
        }
    }
    if (status < 0)
        print_error(t, "mv", status);
    else
        shell_fs_changed(wm);
}

static void command_edit(struct wm *wm, const char *path)
{
    struct terminal *t = &wm->terminal;
    int node = fs_resolve(wm->fs, t->cwd, path);

    if (node == FS_ERR_NOT_FOUND) {
        char name[FS_NAME_MAX];
        int parent;
        int status = split_parent(wm, path, &parent, name);
        node = status < 0 ? status : fs_create(wm->fs, parent, name, FS_FILE);
        if (node >= 0)
            shell_fs_changed(wm);
    }
    if (node < 0) {
        print_error(t, "edit", node);
    } else if (!fs_is_file(wm->fs, node)) {
        print_error(t, "edit", FS_ERR_IS_DIR);
    } else {
        editor_open(wm, node);
    }
}

static void command_net(struct wm *wm)
{
    struct terminal *t = &wm->terminal;
    struct net *net = wm->net;
    char value[24];

    if (!net) {
        terminal_print(t, "Networking is not available.");
        return;
    }
    net_start(net);
    print_field(t, "State:   ", net_state_text(net));
    if (net->state == NET_READY) {
        net_format_ip(net->ip, value, sizeof value);
        print_field(t, "Address: ", value);
        net_format_ip(net->router, value, sizeof value);
        print_field(t, "Router:  ", value);
        net_format_ip(net->dns_server, value, sizeof value);
        print_field(t, "DNS:     ", value);
    } else if (net->state == NET_CONFIGURING) {
        terminal_print(t, "Run net again in a few seconds.");
    }
}

static void command_ping(struct wm *wm, const char *host)
{
    struct terminal *t = &wm->terminal;
    if (!wm->net) {
        terminal_print(t, "Networking is not available.");
    } else if (t->ping_phase != PING_PHASE_NONE) {
        terminal_print(t, "A ping is already running.");
    } else if (wm->browser.loading_tab >= 0) {
        terminal_print(t, "The browser is using the network.");
    } else if (text_len(host) >= PING_HOST_MAX) {
        terminal_print(t, "That host name is too long.");
    } else {
        text_copy(t->ping_host, sizeof t->ping_host, host);
        t->ping_phase = PING_PHASE_NETWORK;
        net_start(wm->net);
    }
}

void terminal_cancel_ping(struct wm *wm)
{
    if (wm->terminal.ping_phase != PING_PHASE_NONE) {
        wm->terminal.ping_phase = PING_PHASE_NONE;
        terminal_print(&wm->terminal, "ping: cancelled");
        wm_invalidate_app(wm, APP_TERMINAL);
    }
}

static void ping_finish(struct wm *wm, const char *message)
{
    wm->terminal.ping_phase = PING_PHASE_NONE;
    terminal_print(&wm->terminal, message);
    wm_invalidate_app(wm, APP_TERMINAL);
}

static void ping_send(struct wm *wm, uint32_t ip)
{
    if (net_ping(wm->net, ip))
        wm->terminal.ping_phase = PING_PHASE_REPLY;
    else
        ping_finish(wm, "ping: could not send");
}

void terminal_tick(struct wm *wm)
{
    struct terminal *t = &wm->terminal;
    struct net *net = wm->net;
    uint32_t ip;

    switch (t->ping_phase) {
    case PING_PHASE_NETWORK:
        if (net->state == NET_FAILED) {
            char message[TERM_COLS + 1];
            text_copy(message, sizeof message, "ping: ");
            text_append(message, sizeof message, net_state_text(net));
            ping_finish(wm, message);
        } else if (net->state == NET_READY) {
            if (net_parse_ip(t->ping_host, &ip))
                ping_send(wm, ip);
            else if (dns_resolve(net, t->ping_host))
                t->ping_phase = PING_PHASE_DNS;
            else
                ping_finish(wm, "ping: could not look up that name");
        }
        break;
    case PING_PHASE_DNS:
        if (net->dns.state == DNS_DONE)
            ping_send(wm, net->dns.address);
        else if (net->dns.state != DNS_WAITING)
            ping_finish(wm, "ping: could not look up that name");
        break;
    case PING_PHASE_REPLY:
        if (net->ping.state == PING_DONE) {
            char message[TERM_COLS + 1];
            net_format_ip(net->ping.target, message, sizeof message);
            text_append(message, sizeof message, ": reply in ");
            text_append_u64(message, sizeof message, net->ping.round_trip);
            text_append(message, sizeof message, " ms");
            wm_trace(wm, "net:ping-ok");
            ping_finish(wm, message);
        } else if (net->ping.state != PING_WAITING) {
            ping_finish(wm, "ping: no reply");
        }
        break;
    default:
        break;
    }
}

static void trace_command(const struct wm *wm, const char *command)
{
    char event[32];
    text_copy(event, sizeof event, "cmd:");
    text_append(event, sizeof event, command);
    wm_trace(wm, event);
}

static void dispatch(struct wm *wm, int argc, char **argv, const char *raw)
{
    struct terminal *t = &wm->terminal;
    const struct sysinfo *info = wm->info;
    const char *command = argv[0];

    if (text_equal(command, "help")) {
        command_help(t);
    } else if (text_equal(command, "about")) {
        terminal_print(t, "Cato OS: a from-scratch x86_64 UEFI system.");
        terminal_print(t, "This desktop runs inside the boot loader.");
        terminal_print(t, "Files are kept in RAM and lost on exit.");
    } else if (text_equal(command, "sysinfo")) {
        print_field(t, "Firmware: ", info->firmware);
        print_field(t, "CPU:      ", info->cpu);
        print_field(t, "Memory:   ", info->memory);
        print_field(t, "Display:  ", info->display);
    } else if (text_equal(command, "time")) {
        print_field(t, "Firmware clock: ", wm->clock);
    } else if (text_equal(command, "clear")) {
        terminal_clear(t);
    } else if (text_equal(command, "echo")) {
        while (*raw == ' ')
            raw++;
        terminal_print(t, text_starts(raw, "echo ") ? raw + 5 : "");
    } else if (text_equal(command, "pwd")) {
        char path[TERM_COLS * 2];
        fs_path(wm->fs, t->cwd, path, sizeof path);
        terminal_print(t, path);
    } else if (text_equal(command, "ls")) {
        command_ls(wm, argc, argv);
    } else if (text_equal(command, "cd")) {
        command_cd(wm, argc, argv);
    } else if (text_equal(command, "files")) {
        wm_open(wm, APP_FILES);
    } else if (text_equal(command, "editor")) {
        wm_open(wm, APP_EDITOR);
    } else if (text_equal(command, "system")) {
        wm_open(wm, APP_SYSTEM);
    } else if (text_equal(command, "browser")) {
        wm_open(wm, APP_BROWSER);
        if (argc > 1)
            browser_navigate(wm, argv[1]);
    } else if (text_equal(command, "net")) {
        command_net(wm);
    } else if (text_equal(command, "ping")) {
        if (argc != 2)
            print_field(t, "Usage: ", "ping HOST");
        else
            command_ping(wm, argv[1]);
    } else if (text_equal(command, "exit")) {
        shell_request_exit(wm);
    } else if (text_equal(command, "mkdir") || text_equal(command, "touch") ||
               text_equal(command, "cat") || text_equal(command, "rm") ||
               text_equal(command, "edit")) {
        if (argc != 2) {
            print_field(t, "Usage: ", text_equal(command, "mkdir") ? "mkdir PATH" : "COMMAND PATH");
        } else if (text_equal(command, "mkdir")) {
            command_create(wm, command, argv[1], FS_DIR);
        } else if (text_equal(command, "touch")) {
            command_create(wm, command, argv[1], FS_FILE);
        } else if (text_equal(command, "cat")) {
            command_cat(wm, argv[1]);
        } else if (text_equal(command, "rm")) {
            command_rm(wm, argv[1]);
        } else {
            command_edit(wm, argv[1]);
        }
    } else if (text_equal(command, "cp") || text_equal(command, "mv")) {
        if (argc != 3)
            print_field(t, "Usage: ", text_equal(command, "cp") ? "cp FROM TO" : "mv FROM TO");
        else if (text_equal(command, "cp"))
            command_cp(wm, argv[1], argv[2]);
        else
            command_mv(wm, argv[1], argv[2]);
    } else {
        print_field(t, "Unknown command: ", command);
        return;
    }
    trace_command(wm, command);
}

static void run_command(struct wm *wm)
{
    struct terminal *t = &wm->terminal;
    char echoed[TERM_COLS + 1];
    char raw[sizeof t->input];
    char parsed[sizeof t->input];
    char *argv[MAX_ARGS];

    text_copy(echoed, sizeof echoed, "> ");
    text_append(echoed, sizeof echoed, t->input);
    terminal_print(t, echoed);

    text_copy(raw, sizeof raw, t->input);
    text_copy(parsed, sizeof parsed, t->input);
    t->input[0] = 0;
    t->input_len = 0;

    int argc = parse_args(parsed, argv);
    if (argc < 0)
        terminal_print(t, "Too many arguments.");
    else if (argc > 0)
        dispatch(wm, argc, argv, raw);
}

void terminal_key(struct wm *wm, int key)
{
    struct terminal *t = &wm->terminal;

    if (key == KEY_ENTER) {
        run_command(wm);
    } else if (key == KEY_BACKSPACE) {
        if (t->input_len > 0)
            t->input[--t->input_len] = 0;
    } else if (key >= 0x20 && key <= 0x7e && t->input_len < (int)sizeof t->input - 1) {
        t->input[t->input_len++] = (char)key;
        t->input[t->input_len] = 0;
    } else {
        return;
    }
    wm_invalidate_app(wm, APP_TERMINAL);
}
