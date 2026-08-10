#include "robu/types.h"
#include "robu/uipc.h"
#include "robu/vfs.h"

#define RAMFS_MAX_FILES 96
#define RAMFS_MAX_BIG_FILES 24
#define RAMFS_MAX_HANDLES 32
#define RAMFS_SYMLINK_MAX_DEPTH 8

#define RAMFS_MAX_DATA (2 * 1024 * 1024)
typedef struct {
    int in_use;
    int is_dir;
    int is_symlink;
    int data_slot;
    uint64_t parent_ino;
    char name[VFS_PATH_MAX];
    char target[VFS_PATH_MAX];
    uint64_t size;
} ramfs_file_t;
typedef struct {
    int in_use;
    int file_idx;
    uint64_t offset;
} ramfs_handle_t;
static ramfs_file_t files[RAMFS_MAX_FILES];
static ramfs_handle_t handles[RAMFS_MAX_HANDLES];
static uint8_t big_data[RAMFS_MAX_BIG_FILES][RAMFS_MAX_DATA];
static int big_data_in_use[RAMFS_MAX_BIG_FILES];
static int alloc_data_slot(void) {
    for (int i = 0; i < RAMFS_MAX_BIG_FILES; i++) {
        if (!big_data_in_use[i]) {
            big_data_in_use[i] = 1;
            return i;
        }
    }
    return -1;
}
static void free_data_slot(int slot) {
    if (slot >= 0) {
        big_data_in_use[slot] = 0;
    }
}
static int name_eq(const char *a, const char *b) {
    for (int i = 0; i < VFS_PATH_MAX; i++) {
        if (a[i] != b[i]) {
            return 0;
        }
        if (a[i] == '\0') {
            return 1;
        }
    }
    return 1;
}
static void set_name(char *dst, const char *src, size_t dst_size) {
    size_t i = 0;
    for (; i < dst_size - 1 && src[i]; i++) {
        dst[i] = src[i];
    }
    dst[i] = '\0';
}
static int find_file(const char *name) {
    for (int i = 0; i < RAMFS_MAX_FILES; i++) {
        if (files[i].in_use && name_eq(files[i].name, name)) {
            return i;
        }
    }
    return -1;
}

static int resolve_path(const char *name) {
    char cur[VFS_PATH_MAX];
    set_name(cur, name, sizeof(cur));
    for (int hop = 0; hop < RAMFS_SYMLINK_MAX_DEPTH; hop++) {
        int idx = find_file(cur);
        if (idx < 0) {
            return -1;
        }
        if (!files[idx].is_symlink) {
            return idx;
        }
        set_name(cur, files[idx].target, sizeof(cur));
    }
    return -1;
}
static int alloc_file(void) {
    for (int i = 0; i < RAMFS_MAX_FILES; i++) {
        if (!files[i].in_use) {
            return i;
        }
    }
    return -1;
}
static int alloc_handle(void) {
    for (int i = 0; i < RAMFS_MAX_HANDLES; i++) {
        if (!handles[i].in_use) {
            return i;
        }
    }
    return -1;
}
static int valid_handle(uint64_t h) {
    return h < RAMFS_MAX_HANDLES && handles[h].in_use;
}
static void parent_path(const char *path, char *out, size_t out_size) {
    size_t len = 0;
    while (path[len]) {
        len++;
    }
    size_t last_slash = 0;
    int found = 0;
    for (size_t i = 0; i < len; i++) {
        if (path[i] == '/') {
            last_slash = i;
            found = 1;
        }
    }
    if (!found) {
        out[0] = '\0';
        return;
    }
    size_t n = last_slash < out_size - 1 ? last_slash : out_size - 1;
    for (size_t i = 0; i < n; i++) {
        out[i] = path[i];
    }
    out[n] = '\0';
}
static uint64_t resolve_parent_ino(const char *path) {
    char parent[VFS_PATH_MAX];
    parent_path(path, parent, sizeof(parent));
    if (parent[0] == '\0') {
        return VFS_ROOT_INO;
    }
    int idx = find_file(parent);
    if (idx < 0) {
        return VFS_ROOT_INO;
    }
    return (uint64_t)idx + 2;
}
static const char *basename_of(const char *path) {
    size_t len = 0;
    while (path[len]) {
        len++;
    }
    size_t last_slash = 0;
    int found = 0;
    for (size_t i = 0; i < len; i++) {
        if (path[i] == '/') {
            last_slash = i;
            found = 1;
        }
    }
    return found ? path + last_slash + 1 : path;
}
static void handle_open(msg_regs_t *m) {
    char name[VFS_PATH_MAX];
    const vfs_open_req_t *req = (const vfs_open_req_t *)m;
    uint64_t flags = req->flags;
    for (int i = 0; i < VFS_PATH_MAX; i++) {
        name[i] = req->name[i];
    }
    vfs_open_reply_t *reply = (vfs_open_reply_t *)m;
    int raw_idx = find_file(name);
    int fidx = (raw_idx >= 0 && files[raw_idx].is_symlink) ? resolve_path(name) : raw_idx;
    if (fidx >= 0 && files[fidx].is_dir) {
        reply->status = VFS_ERR_IS_DIR;
        return;
    }
    if (fidx < 0) {
        if (raw_idx >= 0 || !(flags & VFS_O_CREAT)) {

            reply->status = VFS_ERR_NOT_FOUND;
            return;
        }
        fidx = alloc_file();
        if (fidx < 0) {
            reply->status = VFS_ERR_NO_SPACE;
            return;
        }
        int slot = alloc_data_slot();
        if (slot < 0) {
            reply->status = VFS_ERR_NO_SPACE;
            return;
        }
        files[fidx].in_use = 1;
        files[fidx].is_dir = 0;
        files[fidx].is_symlink = 0;
        files[fidx].data_slot = slot;
        files[fidx].size = 0;
        set_name(files[fidx].name, name, sizeof(files[fidx].name));
        files[fidx].parent_ino = resolve_parent_ino(name);
    } else if (flags & VFS_O_TRUNC) {
        files[fidx].size = 0;
    }
    int hidx = alloc_handle();
    if (hidx < 0) {
        reply->status = VFS_ERR_NO_SPACE;
        return;
    }
    handles[hidx].in_use = 1;
    handles[hidx].file_idx = fidx;
    handles[hidx].offset = (flags & VFS_O_APPEND) ? files[fidx].size : 0;
    reply->status = 0;
    reply->handle = (uint64_t)hidx;
}
static void handle_read(msg_regs_t *m) {
    const vfs_read_req_t *req = (const vfs_read_req_t *)m;
    uint64_t h = req->handle;
    uint64_t len = req->len > VFS_READ_MAX ? VFS_READ_MAX : req->len;
    vfs_read_reply_t *reply = (vfs_read_reply_t *)m;
    if (!valid_handle(h)) {
        reply->status = VFS_ERR_BAD_HANDLE;
        return;
    }
    ramfs_handle_t *hd = &handles[h];
    ramfs_file_t *f = &files[hd->file_idx];
    uint8_t *data = big_data[f->data_slot];
    uint64_t avail = f->size > hd->offset ? f->size - hd->offset : 0;
    uint64_t n = len < avail ? len : avail;
    for (uint64_t i = 0; i < n; i++) {
        reply->data[i] = data[hd->offset + i];
    }
    hd->offset += n;
    reply->status = (int64_t)n;
}
static void handle_write(msg_regs_t *m) {
    const vfs_write_req_t *req = (const vfs_write_req_t *)m;
    uint64_t h = req->handle;
    uint64_t len = req->len > VFS_WRITE_MAX ? VFS_WRITE_MAX : req->len;
    uint8_t data[VFS_WRITE_MAX];
    for (uint64_t i = 0; i < len; i++) {
        data[i] = req->data[i];
    }
    vfs_write_reply_t *reply = (vfs_write_reply_t *)m;
    if (!valid_handle(h)) {
        reply->status = VFS_ERR_BAD_HANDLE;
        return;
    }
    ramfs_handle_t *hd = &handles[h];
    ramfs_file_t *f = &files[hd->file_idx];
    uint8_t *buf = big_data[f->data_slot];
    uint64_t space = RAMFS_MAX_DATA > hd->offset ? RAMFS_MAX_DATA - hd->offset : 0;
    uint64_t n = len < space ? len : space;
    for (uint64_t i = 0; i < n; i++) {
        buf[hd->offset + i] = data[i];
    }
    hd->offset += n;
    if (hd->offset > f->size) {
        f->size = hd->offset;
    }
    reply->status = (int64_t)n;
}
static void handle_close(msg_regs_t *m) {
    const vfs_close_req_t *req = (const vfs_close_req_t *)m;
    vfs_close_reply_t *reply = (vfs_close_reply_t *)m;
    uint64_t h = req->handle;
    if (!valid_handle(h)) {
        reply->status = VFS_ERR_BAD_HANDLE;
        return;
    }
    handles[h].in_use = 0;
    reply->status = 0;
}
static void handle_stat(msg_regs_t *m) {
    char name[VFS_PATH_MAX];
    const vfs_stat_req_t *req = (const vfs_stat_req_t *)m;
    for (int i = 0; i < VFS_PATH_MAX; i++) {
        name[i] = req->name[i];
    }
    vfs_stat_reply_t *reply = (vfs_stat_reply_t *)m;
    if (name[0] == '/' && name[1] == '\0') {
        reply->status = 0;
        reply->size = 0;
        reply->is_dir = 1;
        reply->ino = VFS_ROOT_INO;
        return;
    }
    int raw_idx = find_file(name);
    int fidx = (raw_idx >= 0 && files[raw_idx].is_symlink) ? resolve_path(name) : raw_idx;
    if (fidx < 0) {
        reply->status = VFS_ERR_NOT_FOUND;
        return;
    }
    reply->status = 0;
    reply->size = files[fidx].size;
    reply->is_dir = (uint64_t)files[fidx].is_dir;
    reply->ino = (uint64_t)fidx + 2;
}
static void handle_fstat(msg_regs_t *m) {
    const vfs_fstat_req_t *req = (const vfs_fstat_req_t *)m;
    uint64_t h = req->handle;
    vfs_stat_reply_t *reply = (vfs_stat_reply_t *)m;
    if (!valid_handle(h)) {
        reply->status = VFS_ERR_BAD_HANDLE;
        return;
    }
    reply->status = 0;
    reply->size = files[handles[h].file_idx].size;
    reply->is_dir = (uint64_t)files[handles[h].file_idx].is_dir;
    reply->ino = (uint64_t)handles[h].file_idx + 2;
}
static void handle_readdir(msg_regs_t *m) {
    const vfs_readdir_req_t *req = (const vfs_readdir_req_t *)m;
    uint64_t dir_ino = req->dir_ino;
    uint64_t want = req->index;
    vfs_readdir_reply_t *reply = (vfs_readdir_reply_t *)m;
    uint64_t seen = 0;
    for (int i = 0; i < RAMFS_MAX_FILES; i++) {
        if (!files[i].in_use || files[i].parent_ino != dir_ino) {
            continue;
        }
        if (seen == want) {
            reply->status = 0;
            reply->is_dir = (uint64_t)files[i].is_dir;
            set_name(reply->name, basename_of(files[i].name), sizeof(reply->name));
            return;
        }
        seen++;
    }
    reply->status = VFS_ERR_NOT_FOUND;
}
static void handle_rename(msg_regs_t *m) {
    char oldname[VFS_NAME_MAX], newname[VFS_NAME_MAX];
    const vfs_rename_req_t *req = (const vfs_rename_req_t *)m;
    for (int i = 0; i < VFS_NAME_MAX; i++) {
        oldname[i] = req->oldname[i];
        newname[i] = req->newname[i];
    }
    vfs_rename_reply_t *reply = (vfs_rename_reply_t *)m;
    int fidx = find_file(oldname);
    if (fidx < 0) {
        reply->status = VFS_ERR_NOT_FOUND;
        return;
    }
    if (files[fidx].is_dir) {
        reply->status = VFS_ERR_IS_DIR;
        return;
    }
    int existing = find_file(newname);
    if (existing >= 0) {
        if (files[existing].is_dir) {
            reply->status = VFS_ERR_IS_DIR;
            return;
        }
        if (existing != fidx) {
            free_data_slot(files[existing].data_slot);
            files[existing].in_use = 0;
        }
    }
    set_name(files[fidx].name, newname, sizeof(files[fidx].name));
    files[fidx].parent_ino = resolve_parent_ino(newname);
    reply->status = 0;
}
static void handle_unlink(msg_regs_t *m) {
    char name[VFS_PATH_MAX];
    const vfs_unlink_req_t *req = (const vfs_unlink_req_t *)m;
    for (int i = 0; i < VFS_PATH_MAX; i++) {
        name[i] = req->name[i];
    }
    vfs_unlink_reply_t *reply = (vfs_unlink_reply_t *)m;
    int fidx = find_file(name);
    if (fidx < 0) {
        reply->status = VFS_ERR_NOT_FOUND;
        return;
    }
    if (files[fidx].is_dir) {
        reply->status = VFS_ERR_IS_DIR;
        return;
    }
    free_data_slot(files[fidx].data_slot);
    files[fidx].in_use = 0;
    reply->status = 0;
}
static void handle_symlink(msg_regs_t *m) {
    char name[VFS_NAME_MAX], target[VFS_NAME_MAX];
    const vfs_symlink_req_t *req = (const vfs_symlink_req_t *)m;
    for (int i = 0; i < VFS_NAME_MAX; i++) {
        name[i] = req->name[i];
        target[i] = req->target[i];
    }
    vfs_symlink_reply_t *reply = (vfs_symlink_reply_t *)m;
    if (find_file(name) >= 0) {
        reply->status = VFS_ERR_NOT_SUPPORTED;
        return;
    }
    int idx = alloc_file();
    if (idx < 0) {
        reply->status = VFS_ERR_NO_SPACE;
        return;
    }
    files[idx].in_use = 1;
    files[idx].is_dir = 0;
    files[idx].is_symlink = 1;
    files[idx].data_slot = -1;
    files[idx].size = 0;
    set_name(files[idx].name, name, sizeof(files[idx].name));
    set_name(files[idx].target, target, sizeof(files[idx].target));
    files[idx].parent_ino = resolve_parent_ino(name);
    reply->status = 0;
}
static uint64_t seed_dir(const char *name, uint64_t parent_ino) {
    int idx = alloc_file();
    if (idx < 0) {
        return 0;
    }
    files[idx].in_use = 1;
    files[idx].is_dir = 1;
    files[idx].is_symlink = 0;
    files[idx].data_slot = -1;
    files[idx].parent_ino = parent_ino;
    files[idx].size = 0;
    set_name(files[idx].name, name, sizeof(files[idx].name));
    return (uint64_t)idx + 2;
}
static void seed_fixed_dirs(void) {
    seed_dir("bin", VFS_ROOT_INO);
    seed_dir("etc", VFS_ROOT_INO);
    uint64_t var_ino = seed_dir("var", VFS_ROOT_INO);
    if (var_ino) {
        seed_dir("var/tmp", var_ino);
        seed_dir("var/root", var_ino);
    }
    seed_dir("sbin", VFS_ROOT_INO);
    uint64_t usr_ino = seed_dir("usr", VFS_ROOT_INO);
    if (usr_ino) {
        seed_dir("usr/bin", usr_ino);
        seed_dir("usr/sbin", usr_ino);
    }

    seed_dir("dev", VFS_ROOT_INO);
    seed_dir("proc", VFS_ROOT_INO);
}
void _start(void) {
    msg_regs_t m;
    tid_t from;
    seed_fixed_dirs();
    for (;;) {
        ipc_recv(IPC_TID_ANY, &m, &from);
        switch (m.word[0]) {
        case VFS_OP_OPEN:
            handle_open(&m);
            break;
        case VFS_OP_READ:
            handle_read(&m);
            break;
        case VFS_OP_WRITE:
            handle_write(&m);
            break;
        case VFS_OP_CLOSE:
            handle_close(&m);
            break;
        case VFS_OP_STAT:
            handle_stat(&m);
            break;
        case VFS_OP_FSTAT:
            handle_fstat(&m);
            break;
        case VFS_OP_READDIR:
            handle_readdir(&m);
            break;
        case VFS_OP_RENAME:
            handle_rename(&m);
            break;
        case VFS_OP_UNLINK:
            handle_unlink(&m);
            break;
        case VFS_OP_SYMLINK:
            handle_symlink(&m);
            break;
        default:
            ((vfs_open_reply_t *)&m)->status = VFS_ERR_NOT_FOUND;
            break;
        }
        ipc_send(from, &m);
    }
}
