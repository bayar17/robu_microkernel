#include "robu/types.h"
#include "robu/uipc.h"
#include "robu/ipc.h"
#include "robu/kinfo.h"
#include "robu/vfs.h"

static tid_t devfs_tid = 0;
static int64_t console_h = -1;

static char frame_buf[4096];
static int frame_len = 0;

static void buf_str(const char *s) {
    int l = 0;
    while (s[l] && frame_len < (int)sizeof(frame_buf) - 1) {
        frame_buf[frame_len++] = s[l++];
    }
}

static void buf_num(uint64_t n) {
    if (n == 0) { buf_str("0"); return; }
    char buf[24], rev[24];
    int r = 0, p = 0;
    while (n > 0) { rev[r++] = '0' + (n % 10); n /= 10; }
    while (r > 0) buf[p++] = rev[--r];
    buf[p] = '\0';
    buf_str(buf);
}

/* Transmite o frame inteiro em fatias respeitando VFS_WRITE_MAX (24 bytes por pacote) */
static void flush_frame(void) {
    int sent = 0;
    while (sent < frame_len && console_h >= 0) {
        int chunk = frame_len - sent;
        if (chunk > VFS_WRITE_MAX) chunk = VFS_WRITE_MAX;
        int64_t n = vfs_write(devfs_tid, (uint64_t)console_h, frame_buf + sent, (uint64_t)chunk);
        if (n <= 0) break;
        sent += (int)n;
    }
    frame_len = 0;
}

static int str_eq(const char *a, const char *b) {
    int i = 0;
    while (a[i] && b[i]) {
        if (a[i] != b[i]) return 0;
        i++;
    }
    return a[i] == b[i];
}

static void str_copy(char *dst, const char *src, int max) {
    int i = 0;
    for (; i < max - 1 && src[i]; i++) dst[i] = src[i];
    dst[i] = '\0';
}

static void str_cat(char *dst, const char *src, int max) {
    int d = 0;
    while (dst[d] && d < max - 1) d++;
    int s = 0;
    while (src[s] && d < max - 1) dst[d++] = src[s++];
    dst[d] = '\0';
}

static int str_len(const char *s) {
    int l = 0;
    while (s[l]) l++;
    return l;
}

static void set_raw_mode(int enable) {
    msg_regs_t m = (msg_regs_t){0};
    m.word[0] = 10;
    m.word[1] = enable ? 1 : 0;
    robu_ipc_raw(0, 0, IPC_FLAG_SYS_INFO, &m, NULL);
}

static int read_key(void) {
    uint8_t c;
    int64_t n = vfs_read(devfs_tid, (uint64_t)console_h, &c, 1);
    if (n > 0) return (int)c;
    return -1;
}

static int read_key_wait(int max_tries) {
    for (int i = 0; i < max_tries; i++) {
        int k = read_key();
        if (k >= 0) return k;
        ipc_sleep(1);
    }
    return -1;
}

#define MAX_ENTRIES 128
#define VISIBLE_ROWS 12

typedef struct {
    char name[VFS_NAME_MAX];
    int is_dir;
} entry_t;

static entry_t entries[MAX_ENTRIES];
static int entry_count = 0;

static void go_parent(char *path) {
    int len = str_len(path);
    if (len <= 1) return;
    if (path[len - 1] == '/') path[--len] = '\0';
    int last_slash = -1;
    for (int i = 0; i < len; i++) {
        if (path[i] == '/') last_slash = i;
    }
    if (last_slash <= 0) {
        path[0] = '/'; path[1] = '\0';
    } else {
        path[last_slash] = '\0';
    }
}

static void load_dir(const char *path) {
    entry_count = 0;
    int matched_len = 0;
    const volatile kinfo_page_t *k = kinfo_user();
    tid_t server = kinfo_resolve_mount(k, path, &matched_len);
    if (server == 0) return;

    const char *rel = path + matched_len;

    uint64_t size, ino = 0;
    int is_dir = 0;
    int64_t st = vfs_stat(server, rel, &size, &is_dir, &ino);
    if (st != 0 || ino == 0) {
        ino = VFS_ROOT_INO;
    }

    if (!str_eq(path, "/")) {
        str_copy(entries[entry_count].name, "..", VFS_NAME_MAX);
        entries[entry_count].is_dir = 1;
        entry_count++;
    }

    for (uint64_t idx = 0; idx < MAX_ENTRIES - 1; idx++) {
        char name_buf[VFS_NAME_MAX];
        int item_is_dir = 0;
        int64_t rc = vfs_readdir(server, ino, idx, name_buf, &item_is_dir);
        if (rc != 0) break;

        if (str_eq(name_buf, ".") || str_eq(name_buf, "..")) continue;

        str_copy(entries[entry_count].name, name_buf, VFS_NAME_MAX);
        entries[entry_count].is_dir = item_is_dir;
        entry_count++;
    }
}

static void preview_file(const char *path, const char *filename) {
    frame_len = 0;
    buf_str("\033[2J\033[H\033[33m--- Content of ");
    buf_str(filename);
    buf_str(" ---\033[0m\r\n\r\n");

    char full[VFS_PATH_MAX];
    str_copy(full, path, sizeof(full));
    if (full[str_len(full) - 1] != '/') str_cat(full, "/", sizeof(full));
    str_cat(full, filename, sizeof(full));

    int matched_len = 0;
    tid_t server = kinfo_resolve_mount(kinfo_user(), full, &matched_len);
    if (server != 0) {
        int64_t h = vfs_open(server, full + matched_len, 0);
        if (h >= 0) {
            uint8_t buf[256];
            int64_t n = vfs_read(server, (uint64_t)h, buf, sizeof(buf) - 1);
            if (n > 0) {
                buf[n] = '\0';
                buf_str((char*)buf);
            } else {
                buf_str("(Empty or unreadable file)\r\n");
            }
            vfs_close(server, (uint64_t)h);
        } else {
            buf_str("(Could not open file)\r\n");
        }
    }

    buf_str("\r\n\r\n\033[47;30m [ Press any key to return ] \033[0m\r\n");
    flush_frame();

    while (1) {
        int k = read_key();
        if (k >= 0) break;
        ipc_sleep(1);
    }
}

void _start(void) {
    devfs_tid = (tid_t)kinfo_user()->devfs_tid;
    console_h = vfs_open(devfs_tid, "console", 0);
    if (console_h < 0) ipc_exit(1);

    set_raw_mode(1);

    frame_len = 0;
    buf_str("\033[0m\033[2J\033[H");
    flush_frame();

    char path[VFS_PATH_MAX] = "/";
    int selected = 0;
    int top_index = 0;
    int dirty = 1;

    while (1) {
        if (dirty) {
            load_dir(path);
            if (selected >= entry_count) selected = entry_count > 0 ? entry_count - 1 : 0;

            if (selected < top_index) {
                top_index = selected;
            } else if (selected >= top_index + VISIBLE_ROWS) {
                top_index = selected - VISIBLE_ROWS + 1;
            }

            frame_len = 0;
            buf_str("\033[H");
            buf_str("\033[44;37;1m --- Robu Native TUI File Manager --- \033[0m\033[K\r\n");
            buf_str("\033[36m Path:\033[0m ");
            buf_str(path);
            buf_str("  (");
            buf_num(entry_count);
            buf_str(" items)\033[K\r\n----------------------------------------\033[K\r\n");

            int lines_printed = 0;
            if (entry_count == 0) {
                buf_str("  (Empty directory)\033[K\r\n");
                lines_printed++;
            } else {
                int end_index = top_index + VISIBLE_ROWS;
                if (end_index > entry_count) end_index = entry_count;

                if (top_index > 0) {
                    buf_str("  \033[33m^ ... (scroll up) ...\033[0m\033[K\r\n");
                    lines_printed++;
                }

                for (int i = top_index; i < end_index; i++) {
                    if (i == selected) buf_str("\033[47;30m> ");
                    else buf_str("  ");

                    if (entries[i].is_dir) {
                        buf_str("\033[34m[DIR ]\033[0m ");
                    } else {
                        buf_str("\033[32m[FILE]\033[0m ");
                    }
                    buf_str(entries[i].name);

                    if (i == selected) buf_str("\033[0m\033[K\r\n");
                    else buf_str("\033[K\r\n");
                    lines_printed++;
                }

                if (end_index < entry_count) {
                    buf_str("  \033[33mv ... (scroll down) ...\033[0m\033[K\r\n");
                    lines_printed++;
                }
            }

            while (lines_printed < VISIBLE_ROWS + 1) {
                buf_str("\033[K\r\n");
                lines_printed++;
            }

            buf_str("----------------------------------------\033[K\r\n");
            buf_str("[Up/Down/w/s] Nav   [p/Left] Parent   [ENTER/Right] Open   [q] Quit\033[K\r\n");

            flush_frame();
            dirty = 0;
        }

        int c = read_key();
        if (c < 0) {
            ipc_sleep(1);
            continue;
        }

        if (c == 'q' || c == 'Q') break;

        if (c == 'p' || c == 'P' || c == 8 || c == 127) {
            go_parent(path);
            selected = 0;
            top_index = 0;
            dirty = 1;
        }
        else if (c == 'w' || c == 'W') {
            if (selected > 0) { selected--; dirty = 1; }
        } else if (c == 's' || c == 'S') {
            if (selected < entry_count - 1) { selected++; dirty = 1; }
        } else if (c == 27) {
            int c2 = read_key_wait(10);
            if (c2 == '[') {
                int c3 = read_key_wait(10);
                if (c3 == 'A' && selected > 0) {
                    selected--; dirty = 1;
                }
                else if (c3 == 'B' && selected < entry_count - 1) {
                    selected++; dirty = 1;
                }
                else if (c3 == 'C') {
                    if (entry_count > 0 && entries[selected].is_dir) {
                        if (str_eq(entries[selected].name, "..")) {
                            go_parent(path);
                        } else {
                            if (path[str_len(path) - 1] != '/') str_cat(path, "/", sizeof(path));
                            str_cat(path, entries[selected].name, sizeof(path));
                        }
                        selected = 0; top_index = 0; dirty = 1;
                    } else if (entry_count > 0) {
                        preview_file(path, entries[selected].name);
                        dirty = 1;
                    }
                }
                else if (c3 == 'D') {
                    go_parent(path);
                    selected = 0; top_index = 0; dirty = 1;
                }
            }
        } else if (c == '\n' || c == '\r') {
            if (entry_count > 0) {
                if (entries[selected].is_dir) {
                    if (str_eq(entries[selected].name, "..")) {
                        go_parent(path);
                    } else {
                        if (path[str_len(path) - 1] != '/') str_cat(path, "/", sizeof(path));
                        str_cat(path, entries[selected].name, sizeof(path));
                    }
                    selected = 0;
                    top_index = 0;
                    dirty = 1;
                } else {
                    preview_file(path, entries[selected].name);
                    dirty = 1;
                }
            }
        }
    }

    frame_len = 0;
    buf_str("\033[0m\033[2J\033[H");
    flush_frame();

    set_raw_mode(0);
    vfs_close(devfs_tid, (uint64_t)console_h);
    ipc_exit(0);
}
