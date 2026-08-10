#include "robu/types.h"
#include "robu/uipc.h"
#include "robu/vfs.h"

static vaddr_t heap_bump = 0x40000000ULL; 

// Alocação Ultra-Rápida: Deixa o Page Fault acontecer natural e puramente sob demanda!
static void *ram_alloc(size_t sz) {
    sz = (sz + 7) & ~7ULL;
    void *ptr = (void*)heap_bump;
    heap_bump += sz;
    return ptr;
}

typedef struct {
    int in_use;
    int is_dir;
    int is_symlink;
    uint64_t parent_ino;
    char name[VFS_PATH_MAX];
    char target[VFS_PATH_MAX];
    uint64_t size;
    
    uint64_t capacity;
    uint8_t *data;
} ramfs_file_t;

typedef struct {
    int in_use;
    int file_idx;
    uint64_t offset;
} ramfs_handle_t;

static ramfs_file_t *files = NULL;
static uint32_t max_files = 0;

static ramfs_handle_t *handles = NULL;
static uint32_t max_handles = 0;

static int name_eq(const char *a, const char *b) {
    for (int i = 0; i < VFS_PATH_MAX; i++) {
        if (a[i] != b[i]) return 0;
        if (a[i] == '\0') return 1;
    }
    return 1;
}

static void set_name(char *dst, const char *src, size_t dst_size) {
    size_t i = 0;
    for (; i < dst_size - 1 && src[i]; i++) dst[i] = src[i];
    dst[i] = '\0';
}

static int find_file(const char *name) {
    for (uint32_t i = 0; i < max_files; i++) {
        if (files[i].in_use && name_eq(files[i].name, name)) return i;
    }
    return -1;
}

static int resolve_path(const char *name) {
    char cur[VFS_PATH_MAX];
    set_name(cur, name, sizeof(cur));
    for (int hop = 0; hop < 8; hop++) {
        int idx = find_file(cur);
        if (idx < 0) return -1;
        if (!files[idx].is_symlink) return idx;
        set_name(cur, files[idx].target, sizeof(cur));
    }
    return -1;
}

static int alloc_file(void) {
    for (uint32_t i = 0; i < max_files; i++) {
        if (!files[i].in_use) return i;
    }
    uint32_t new_max = max_files == 0 ? 64 : max_files * 2;
    ramfs_file_t *new_files = (ramfs_file_t*)ram_alloc(new_max * sizeof(ramfs_file_t));
    if (files) {
        for (uint32_t i = 0; i < max_files; i++) new_files[i] = files[i];
    }
    for (uint32_t i = max_files; i < new_max; i++) new_files[i].in_use = 0;
    
    int ret_idx = max_files;
    files = new_files;
    max_files = new_max;
    return ret_idx;
}

static int alloc_handle(void) {
    for (uint32_t i = 0; i < max_handles; i++) {
        if (!handles[i].in_use) return i;
    }
    uint32_t new_max = max_handles == 0 ? 32 : max_handles * 2;
    ramfs_handle_t *new_hdls = (ramfs_handle_t*)ram_alloc(new_max * sizeof(ramfs_handle_t));
    if (handles) {
        for (uint32_t i = 0; i < max_handles; i++) new_hdls[i] = handles[i];
    }
    for (uint32_t i = max_handles; i < new_max; i++) new_hdls[i].in_use = 0;
    
    int ret_idx = max_handles;
    handles = new_hdls;
    max_handles = new_max;
    return ret_idx;
}

static int valid_handle(uint64_t h) {
    return h < max_handles && handles[h].in_use;
}

static void parent_path(const char *path, char *out, size_t out_size) {
    size_t len = 0;
    while (path[len]) len++;
    size_t last_slash = 0;
    int found = 0;
    for (size_t i = 0; i < len; i++) {
        if (path[i] == '/') { last_slash = i; found = 1; }
    }
    if (!found) { out[0] = '\0'; return; }
    size_t n = last_slash < out_size - 1 ? last_slash : out_size - 1;
    for (size_t i = 0; i < n; i++) out[i] = path[i];
    out[n] = '\0';
}

static uint64_t resolve_parent_ino(const char *path) {
    char parent[VFS_PATH_MAX];
    parent_path(path, parent, sizeof(parent));
    if (parent[0] == '\0') return VFS_ROOT_INO;
    int idx = find_file(parent);
    if (idx < 0) return VFS_ROOT_INO;
    return (uint64_t)idx + 2;
}

static int resolve_parent_checked(const char *path, uint64_t *ino_out) {
    char parent[VFS_PATH_MAX];
    parent_path(path, parent, sizeof(parent));
    if (parent[0] == '\0') { *ino_out = VFS_ROOT_INO; return 1; }
    int idx = find_file(parent);
    if (idx < 0 || !files[idx].is_dir) return 0;
    *ino_out = (uint64_t)idx + 2;
    return 1;
}

static const char *basename_of(const char *path) {
    size_t len = 0;
    while (path[len]) len++;
    size_t last_slash = 0;
    int found = 0;
    for (size_t i = 0; i < len; i++) {
        if (path[i] == '/') { last_slash = i; found = 1; }
    }
    return found ? path + last_slash + 1 : path;
}

static void handle_open(msg_regs_t *m) {
    char name[VFS_PATH_MAX];
    const vfs_open_req_t *req = (const vfs_open_req_t *)m;
    uint64_t flags = req->flags;
    for (int i = 0; i < VFS_PATH_MAX; i++) name[i] = req->name[i];
    vfs_open_reply_t *reply = (vfs_open_reply_t *)m;
    
    int raw_idx = find_file(name);
    int fidx = (raw_idx >= 0 && files[raw_idx].is_symlink) ? resolve_path(name) : raw_idx;
    if (fidx >= 0 && files[fidx].is_dir) { reply->status = VFS_ERR_IS_DIR; return; }
    
    if (fidx < 0) {
        if (raw_idx >= 0 || !(flags & VFS_O_CREAT)) { reply->status = VFS_ERR_NOT_FOUND; return; }
        fidx = alloc_file();
        files[fidx].in_use = 1;
        files[fidx].is_dir = 0;
        files[fidx].is_symlink = 0;
        files[fidx].size = 0;
        files[fidx].capacity = 0;
        files[fidx].data = NULL;
        set_name(files[fidx].name, name, sizeof(files[fidx].name));
        files[fidx].parent_ino = resolve_parent_ino(name);
    } else if (flags & VFS_O_TRUNC) {
        files[fidx].size = 0;
    }
    int hidx = alloc_handle();
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
    if (!valid_handle(h)) { reply->status = VFS_ERR_BAD_HANDLE; return; }
    
    ramfs_handle_t *hd = &handles[h];
    ramfs_file_t *f = &files[hd->file_idx];
    
    uint64_t avail = f->size > hd->offset ? f->size - hd->offset : 0;
    uint64_t n = len < avail ? len : avail;
    if (f->data) {
        for (uint64_t i = 0; i < n; i++) reply->data[i] = f->data[hd->offset + i];
    }
    hd->offset += n;
    reply->status = (int64_t)n;
}

static void handle_write(msg_regs_t *m) {
    const vfs_write_req_t *req = (const vfs_write_req_t *)m;
    uint64_t h = req->handle;
    uint64_t len = req->len > VFS_WRITE_MAX ? VFS_WRITE_MAX : req->len;
    uint8_t data[VFS_WRITE_MAX];
    for (uint64_t i = 0; i < len; i++) data[i] = req->data[i];
    vfs_write_reply_t *reply = (vfs_write_reply_t *)m;
    if (!valid_handle(h)) { reply->status = VFS_ERR_BAD_HANDLE; return; }
    
    ramfs_handle_t *hd = &handles[h];
    ramfs_file_t *f = &files[hd->file_idx];
    
    // Otimização: Crescimento em blocos de 4KB com cópia em Palavras de 64-bits
    if (hd->offset + len > f->capacity) {
        uint64_t new_cap = f->capacity == 0 ? 4096 : f->capacity * 2;
        while (new_cap < hd->offset + len) new_cap *= 2;
        
        uint8_t *new_data = (uint8_t*)ram_alloc(new_cap);
        if (f->data && f->size > 0) {
            uint64_t *src64 = (uint64_t*)f->data;
            uint64_t *dst64 = (uint64_t*)new_data;
            uint64_t words = f->size / 8;
            for (uint64_t i = 0; i < words; i++) dst64[i] = src64[i];
            for (uint64_t i = words * 8; i < f->size; i++) new_data[i] = f->data[i];
        }
        f->data = new_data;
        f->capacity = new_cap;
    }
    
    for (uint64_t i = 0; i < len; i++) {
        f->data[hd->offset + i] = data[i];
    }
    hd->offset += len;
    if (hd->offset > f->size) f->size = hd->offset;
    reply->status = (int64_t)len;
}

static void handle_close(msg_regs_t *m) {
    const vfs_close_req_t *req = (const vfs_close_req_t *)m;
    vfs_close_reply_t *reply = (vfs_close_reply_t *)m;
    if (!valid_handle(req->handle)) { reply->status = VFS_ERR_BAD_HANDLE; return; }
    handles[req->handle].in_use = 0;
    reply->status = 0;
}

static void handle_stat(msg_regs_t *m) {
    char name[VFS_PATH_MAX];
    const vfs_stat_req_t *req = (const vfs_stat_req_t *)m;
    for (int i = 0; i < VFS_PATH_MAX; i++) name[i] = req->name[i];
    vfs_stat_reply_t *reply = (vfs_stat_reply_t *)m;
    if (name[0] == '/' && name[1] == '\0') {
        reply->status = 0; reply->size = 0; reply->is_dir = 1; reply->ino = VFS_ROOT_INO;
        return;
    }
    int raw_idx = find_file(name);
    int fidx = (raw_idx >= 0 && files[raw_idx].is_symlink) ? resolve_path(name) : raw_idx;
    if (fidx < 0) { reply->status = VFS_ERR_NOT_FOUND; return; }
    reply->status = 0;
    reply->size = files[fidx].size;
    reply->is_dir = (uint64_t)files[fidx].is_dir;
    reply->ino = (uint64_t)fidx + 2;
}

static void handle_fstat(msg_regs_t *m) {
    const vfs_fstat_req_t *req = (const vfs_fstat_req_t *)m;
    uint64_t h = req->handle;
    vfs_stat_reply_t *reply = (vfs_stat_reply_t *)m;
    if (!valid_handle(h)) { reply->status = VFS_ERR_BAD_HANDLE; return; }
    reply->status = 0;
    reply->size = files[handles[h].file_idx].size;
    reply->is_dir = (uint64_t)files[handles[h].file_idx].is_dir;
    reply->ino = (uint64_t)files[handles[h].file_idx].is_dir;
}

static void handle_readdir(msg_regs_t *m) {
    const vfs_readdir_req_t *req = (const vfs_readdir_req_t *)m;
    uint64_t want = req->index;
    vfs_readdir_reply_t *reply = (vfs_readdir_reply_t *)m;
    uint64_t seen = 0;
    for (uint32_t i = 0; i < max_files; i++) {
        if (!files[i].in_use || files[i].parent_ino != req->dir_ino) continue;
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
        oldname[i] = req->oldname[i]; newname[i] = req->newname[i];
    }
    vfs_rename_reply_t *reply = (vfs_rename_reply_t *)m;
    int fidx = find_file(oldname);
    if (fidx < 0) { reply->status = VFS_ERR_NOT_FOUND; return; }
    if (files[fidx].is_dir) { reply->status = VFS_ERR_IS_DIR; return; }
    int existing = find_file(newname);
    if (existing >= 0) {
        if (files[existing].is_dir) { reply->status = VFS_ERR_IS_DIR; return; }
        if (existing != fidx) files[existing].in_use = 0;
    }
    set_name(files[fidx].name, newname, sizeof(files[fidx].name));
    files[fidx].parent_ino = resolve_parent_ino(newname);
    reply->status = 0;
}

static void handle_unlink(msg_regs_t *m) {
    char name[VFS_PATH_MAX];
    const vfs_unlink_req_t *req = (const vfs_unlink_req_t *)m;
    for (int i = 0; i < VFS_PATH_MAX; i++) name[i] = req->name[i];
    vfs_unlink_reply_t *reply = (vfs_unlink_reply_t *)m;
    int fidx = find_file(name);
    if (fidx < 0) { reply->status = VFS_ERR_NOT_FOUND; return; }
    if (files[fidx].is_dir) { reply->status = VFS_ERR_IS_DIR; return; }
    files[fidx].in_use = 0;
    reply->status = 0;
}

static void handle_symlink(msg_regs_t *m) {
    char name[VFS_NAME_MAX], target[VFS_NAME_MAX];
    const vfs_symlink_req_t *req = (const vfs_symlink_req_t *)m;
    for (int i = 0; i < VFS_NAME_MAX; i++) {
        name[i] = req->name[i]; target[i] = req->target[i];
    }
    vfs_symlink_reply_t *reply = (vfs_symlink_reply_t *)m;
    if (find_file(name) >= 0) { reply->status = VFS_ERR_NOT_SUPPORTED; return; }
    int idx = alloc_file();
    files[idx].in_use = 1;
    files[idx].is_dir = 0;
    files[idx].is_symlink = 1;
    files[idx].size = 0;
    set_name(files[idx].name, name, sizeof(files[idx].name));
    set_name(files[idx].target, target, sizeof(files[idx].target));
    files[idx].parent_ino = resolve_parent_ino(name);
    reply->status = 0;
}

static void handle_mkdir(msg_regs_t *m) {
    char name[VFS_PATH_MAX];
    const vfs_mkdir_req_t *req = (const vfs_mkdir_req_t *)m;
    for (int i = 0; i < VFS_PATH_MAX; i++) name[i] = req->name[i];
    vfs_mkdir_reply_t *reply = (vfs_mkdir_reply_t *)m;
    if (find_file(name) >= 0) { reply->status = VFS_ERR_EXISTS; return; }
    uint64_t parent_ino;
    if (!resolve_parent_checked(name, &parent_ino)) { reply->status = VFS_ERR_NOT_FOUND; return; }
    int idx = alloc_file();
    files[idx].in_use = 1;
    files[idx].is_dir = 1;
    files[idx].is_symlink = 0;
    files[idx].size = 0;
    files[idx].capacity = 0;
    files[idx].data = NULL;
    set_name(files[idx].name, name, sizeof(files[idx].name));
    files[idx].parent_ino = parent_ino;
    reply->status = 0;
}

static void handle_rmdir(msg_regs_t *m) {
    char name[VFS_PATH_MAX];
    const vfs_rmdir_req_t *req = (const vfs_rmdir_req_t *)m;
    for (int i = 0; i < VFS_PATH_MAX; i++) name[i] = req->name[i];
    vfs_rmdir_reply_t *reply = (vfs_rmdir_reply_t *)m;
    int fidx = find_file(name);
    if (fidx < 0) { reply->status = VFS_ERR_NOT_FOUND; return; }
    if (!files[fidx].is_dir) { reply->status = VFS_ERR_NOT_DIR; return; }
    uint64_t ino = (uint64_t)fidx + 2;
    for (uint32_t i = 0; i < max_files; i++) {
        if (files[i].in_use && files[i].parent_ino == ino) { reply->status = VFS_ERR_NOT_EMPTY; return; }
    }
    files[fidx].in_use = 0;
    reply->status = 0;
}

static void handle_link(msg_regs_t *m) {
    char oldname[VFS_NAME_MAX], newname[VFS_NAME_MAX];
    const vfs_link_req_t *req = (const vfs_link_req_t *)m;
    for (int i = 0; i < VFS_NAME_MAX; i++) {
        oldname[i] = req->oldname[i]; newname[i] = req->newname[i];
    }
    vfs_link_reply_t *reply = (vfs_link_reply_t *)m;
    int raw_idx = find_file(oldname);
    int fidx = (raw_idx >= 0 && files[raw_idx].is_symlink) ? resolve_path(oldname) : raw_idx;
    if (fidx < 0) { reply->status = VFS_ERR_NOT_FOUND; return; }
    if (files[fidx].is_dir) { reply->status = VFS_ERR_IS_DIR; return; }
    if (find_file(newname) >= 0) { reply->status = VFS_ERR_EXISTS; return; }
    uint64_t parent_ino;
    if (!resolve_parent_checked(newname, &parent_ino)) { reply->status = VFS_ERR_NOT_FOUND; return; }
    uint64_t size = files[fidx].size;
    uint8_t *srcdata = files[fidx].data;
    int idx = alloc_file();
    files[idx].in_use = 1;
    files[idx].is_dir = 0;
    files[idx].is_symlink = 0;
    files[idx].size = size;
    files[idx].capacity = size;
    files[idx].data = size ? (uint8_t*)ram_alloc(size) : NULL;
    for (uint64_t i = 0; i < size; i++) files[idx].data[i] = srcdata[i];
    set_name(files[idx].name, newname, sizeof(files[idx].name));
    files[idx].parent_ino = parent_ino;
    reply->status = 0;
}

static void handle_mknod(msg_regs_t *m) {
    char name[VFS_NAME_MAX];
    const vfs_mknod_req_t *req = (const vfs_mknod_req_t *)m;
    for (int i = 0; i < VFS_NAME_MAX; i++) name[i] = req->name[i];
    vfs_mknod_reply_t *reply = (vfs_mknod_reply_t *)m;
    if (find_file(name) >= 0) { reply->status = VFS_ERR_EXISTS; return; }
    uint64_t parent_ino;
    if (!resolve_parent_checked(name, &parent_ino)) { reply->status = VFS_ERR_NOT_FOUND; return; }
    int idx = alloc_file();
    files[idx].in_use = 1;
    files[idx].is_dir = 0;
    files[idx].is_symlink = 0;
    files[idx].size = 0;
    files[idx].capacity = 0;
    files[idx].data = NULL;
    set_name(files[idx].name, name, sizeof(files[idx].name));
    files[idx].parent_ino = parent_ino;
    reply->status = 0;
}

static uint64_t seed_dir(const char *name, uint64_t parent_ino) {
    int idx = alloc_file();
    files[idx].in_use = 1;
    files[idx].is_dir = 1;
    files[idx].is_symlink = 0;
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
        seed_dir("var/lock", var_ino);
        seed_dir("var/lib", var_ino);
        seed_dir("var/run", var_ino);
    }
    seed_dir("sbin", VFS_ROOT_INO);
    uint64_t usr_ino = seed_dir("usr", VFS_ROOT_INO);
    if (usr_ino) { seed_dir("usr/bin", usr_ino); seed_dir("usr/sbin", usr_ino); }
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
        case VFS_OP_OPEN:    handle_open(&m); break;
        case VFS_OP_READ:    handle_read(&m); break;
        case VFS_OP_WRITE:   handle_write(&m); break;
        case VFS_OP_CLOSE:   handle_close(&m); break;
        case VFS_OP_STAT:    handle_stat(&m); break;
        case VFS_OP_FSTAT:   handle_fstat(&m); break;
        case VFS_OP_READDIR: handle_readdir(&m); break;
        case VFS_OP_RENAME:  handle_rename(&m); break;
        case VFS_OP_UNLINK:  handle_unlink(&m); break;
        case VFS_OP_SYMLINK: handle_symlink(&m); break;
        case VFS_OP_MKDIR:   handle_mkdir(&m); break;
        case VFS_OP_RMDIR:   handle_rmdir(&m); break;
        case VFS_OP_LINK:    handle_link(&m); break;
        case VFS_OP_MKNOD:   handle_mknod(&m); break;
        default: ((vfs_open_reply_t *)&m)->status = VFS_ERR_NOT_FOUND; break;
        }
        ipc_send(from, &m);
    }
}
