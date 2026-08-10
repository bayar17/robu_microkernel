#include "robu/types.h"
#include "robu/uipc.h"
#include "robu/ipc.h"
#include "robu/vfs.h"
#define BOOTFS_MAX_HANDLES 8
#define BOOTFS_NAME_MAX 24
#define BOOTFS_READ_CHUNK 40
typedef struct {
    int in_use;
    char name[BOOTFS_NAME_MAX];
    uint64_t offset;
    uint64_t size;
} bootfs_handle_t;
static bootfs_handle_t handles[BOOTFS_MAX_HANDLES];
static int alloc_handle(void) {
    for (int i = 0; i < BOOTFS_MAX_HANDLES; i++) {
        if (!handles[i].in_use) {
            return i;
        }
    }
    return -1;
}
static int valid_handle(uint64_t h) {
    return h < BOOTFS_MAX_HANDLES && handles[h].in_use;
}
static void pack_name(uint64_t *words, const char *name) {
    uint8_t buf[BOOTFS_NAME_MAX] = {0};
    int i = 0;
    for (; i < BOOTFS_NAME_MAX - 1 && name[i]; i++) {
        buf[i] = (uint8_t)name[i];
    }
    for (int w = 0; w < 3; w++) {
        words[w] = 0;
        for (int b = 0; b < 8; b++) {
            words[w] |= ((uint64_t)buf[w * 8 + b]) << (8 * b);
        }
    }
}
static int kernel_stat(const char *name, uint64_t *size_out) {
    msg_regs_t m = (msg_regs_t){0};
    m.word[0] = 0;
    uint64_t words[3];
    pack_name(words, name);
    m.word[1] = words[0];
    m.word[2] = words[1];
    m.word[3] = words[2];
    int64_t rc = robu_ipc_raw(0, 0, IPC_FLAG_BOOTFS, &m, NULL);
    if (rc != IPC_ERR_NONE) {
        return -1;
    }
    *size_out = m.word[0];
    return 0;
}
static int kernel_readdir(uint64_t index, char *name_out, uint64_t *size_out) {
    msg_regs_t m = (msg_regs_t){0};
    m.word[0] = 1;
    m.word[1] = index;
    int64_t rc = robu_ipc_raw(0, 0, IPC_FLAG_BOOTFS, &m, NULL);
    if (rc != IPC_ERR_NONE) {
        return -1;
    }
    *size_out = m.word[0];
    uint64_t words[3] = { m.word[1], m.word[2], m.word[3] };
    const uint8_t *bytes = (const uint8_t *)words;
    int i = 0;
    for (; i < BOOTFS_NAME_MAX - 1 && bytes[i]; i++) {
        name_out[i] = (char)bytes[i];
    }
    name_out[i] = '\0';
    return 0;
}
static int64_t kernel_read(const char *name, uint64_t offset, uint64_t len, uint8_t *out) {
    msg_regs_t m = (msg_regs_t){0};
    m.word[0] = 2;
    uint64_t words[3];
    pack_name(words, name);
    m.word[1] = words[0];
    m.word[2] = words[1];
    m.word[3] = words[2];
    m.word[4] = offset;
    m.word[5] = len > BOOTFS_READ_CHUNK ? BOOTFS_READ_CHUNK : len;
    int64_t n = robu_ipc_raw(0, 0, IPC_FLAG_BOOTFS, &m, NULL);
    if (n > 0) {
        uint64_t words6[5] = { m.word[0], m.word[1], m.word[2], m.word[3], m.word[4] };
        const uint8_t *bytes = (const uint8_t *)words6;
        for (int64_t i = 0; i < n; i++) {
            out[i] = bytes[i];
        }
    }
    return n;
}
static void handle_open(msg_regs_t *m) {
    char name[VFS_PATH_MAX];
    const vfs_open_req_t *req = (const vfs_open_req_t *)m;
    for (int i = 0; i < VFS_PATH_MAX; i++) {
        name[i] = req->name[i];
    }
    vfs_open_reply_t *reply = (vfs_open_reply_t *)m;
    uint64_t size;
    if (kernel_stat(name, &size) != 0) {
        reply->status = VFS_ERR_NOT_FOUND;
        return;
    }
    int hidx = alloc_handle();
    if (hidx < 0) {
        reply->status = VFS_ERR_NO_SPACE;
        return;
    }
    handles[hidx].in_use = 1;
    handles[hidx].offset = 0;
    handles[hidx].size = size;
    int i = 0;
    for (; i < BOOTFS_NAME_MAX - 1 && name[i]; i++) {
        handles[hidx].name[i] = name[i];
    }
    handles[hidx].name[i] = '\0';
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
    bootfs_handle_t *hd = &handles[h];
    int64_t n = kernel_read(hd->name, hd->offset, len, reply->data);
    if (n < 0) {
        reply->status = VFS_ERR_NOT_FOUND;
        return;
    }
    hd->offset += (uint64_t)n;
    reply->status = n;
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
    if (name[0] == '\0') {

        reply->status = 0;
        reply->size = 0;
        reply->is_dir = 1;
        reply->ino = VFS_ROOT_INO;
        return;
    }
    uint64_t size;
    if (kernel_stat(name, &size) != 0) {
        reply->status = VFS_ERR_NOT_FOUND;
        return;
    }
    reply->status = 0;
    reply->size = size;
    reply->is_dir = 0;
    reply->ino = 0;
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
    reply->size = handles[h].size;
    reply->is_dir = 0;
    reply->ino = h + 1;
}
static void handle_readdir(msg_regs_t *m) {
    const vfs_readdir_req_t *req = (const vfs_readdir_req_t *)m;
    uint64_t index = req->index;
    vfs_readdir_reply_t *reply = (vfs_readdir_reply_t *)m;
    char name[BOOTFS_NAME_MAX];
    uint64_t size;
    if (kernel_readdir(index, name, &size) != 0) {
        reply->status = VFS_ERR_NOT_FOUND;
        return;
    }
    reply->status = 0;
    reply->is_dir = 0;
    int i = 0;
    for (; i < VFS_NAME_MAX - 1 && name[i]; i++) {
        reply->name[i] = name[i];
    }
    reply->name[i] = '\0';
}
void _start(void) {
    msg_regs_t m;
    tid_t from;
    for (;;) {
        ipc_recv(IPC_TID_ANY, &m, &from);
        switch (m.word[0]) {
        case VFS_OP_OPEN:
            handle_open(&m);
            break;
        case VFS_OP_READ:
            handle_read(&m);
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
        case VFS_OP_WRITE:
        case VFS_OP_RENAME:
        case VFS_OP_UNLINK:
            m.word[0] = (uint64_t)(int64_t)VFS_ERR_NOT_SUPPORTED;
            break;
        default:
            m.word[0] = (uint64_t)(int64_t)VFS_ERR_NOT_FOUND;
            break;
        }
        ipc_send(from, &m);
    }
}
