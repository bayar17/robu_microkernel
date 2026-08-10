#include "robu/types.h"
#include "robu/uipc.h"
#include "robu/vfs.h"
typedef enum {
    DEV_CONSOLE = 0,
    DEV_NULL = 1,
    DEV_ZERO = 2,
    DEV_RANDOM = 3,
} dev_id_t;
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
static int resolve_name(const char *name, dev_id_t *out) {
    if (name_eq(name, "console")) { *out = DEV_CONSOLE; return 0; }
    if (name_eq(name, "null"))    { *out = DEV_NULL;    return 0; }
    if (name_eq(name, "zero"))    { *out = DEV_ZERO;    return 0; }
    if (name_eq(name, "random"))  { *out = DEV_RANDOM;  return 0; }
    return -1;
}
static int valid_handle(uint64_t h) {
    return h == DEV_CONSOLE || h == DEV_NULL || h == DEV_ZERO || h == DEV_RANDOM;
}
static int devfs_kernel_console_write(const uint8_t *buf, uint64_t len) {
    msg_regs_t m;
    uint64_t clamped = len > 40 ? 40 : len;
    m.word[0] = clamped;
    uint64_t words[5] = {0, 0, 0, 0, 0};
    uint8_t *bytes = (uint8_t *)words;
    for (uint64_t i = 0; i < clamped; i++) {
        bytes[i] = buf[i];
    }
    m.word[1] = words[0];
    m.word[2] = words[1];
    m.word[3] = words[2];
    m.word[4] = words[3];
    m.word[5] = words[4];
    return (int)robu_ipc_raw(0, 0, IPC_FLAG_CONSOLE_WRITE, &m, NULL);
}
static int devfs_kernel_console_read(uint8_t *buf, uint64_t max) {
    msg_regs_t m = (msg_regs_t){0};
    int64_t rc = robu_ipc_raw(0, 0, IPC_FLAG_CONSOLE_READ, &m, NULL);
    if (rc != IPC_ERR_NONE) {
        return -1;
    }
    uint64_t n = m.word[0];
    if (n > max) {
        n = max;
    }
    uint64_t words[5] = { m.word[1], m.word[2], m.word[3], m.word[4], m.word[5] };
    const uint8_t *bytes = (const uint8_t *)words;
    for (uint64_t i = 0; i < n; i++) {
        buf[i] = bytes[i];
    }
    return (int)n;
}
#define CONSOLE_LOCAL_BUF_SIZE 64
static uint8_t console_local_buf[CONSOLE_LOCAL_BUF_SIZE];
static uint32_t console_local_head, console_local_tail;
static int rdrand_available;
static void check_rdrand(void) {
    uint32_t ecx;
    asm volatile("cpuid" : "=c"(ecx) : "a"(1) : "ebx", "edx");
    rdrand_available = (ecx >> 30) & 1;
}
static int rdrand64(uint64_t *out) {
    for (int attempt = 0; attempt < 10; attempt++) {
        uint64_t v;
        uint8_t ok;
        asm volatile("rdrand %0; setc %1" : "=r"(v), "=qm"(ok));
        if (ok) {
            *out = v;
            return 1;
        }
    }
    return 0;
}
static void handle_open(msg_regs_t *m) {
    char name[VFS_PATH_MAX];
    const vfs_open_req_t *req = (const vfs_open_req_t *)m;
    for (int i = 0; i < VFS_PATH_MAX; i++) {
        name[i] = req->name[i];
    }
    dev_id_t id;
    vfs_open_reply_t *reply = (vfs_open_reply_t *)m;
    if (resolve_name(name, &id) != 0) {
        reply->status = VFS_ERR_NOT_FOUND;
        return;
    }
    reply->status = 0;
    reply->handle = (uint64_t)id;
}
static int console_fg_read_allowed(tid_t from) {
    msg_regs_t q = (msg_regs_t){0};
    q.word[0] = SYS_INFO_CAT_TCGETPGRP;
    robu_ipc_raw(0, 0, IPC_FLAG_SYS_INFO, &q, NULL);
    tid_t fg = (tid_t)q.word[0];
    if (fg == 0) {
        return 1;
    }
    q = (msg_regs_t){0};
    q.word[0] = SYS_INFO_CAT_GETPGID;
    q.word[1] = from;
    robu_ipc_raw(0, 0, IPC_FLAG_SYS_INFO, &q, NULL);
    tid_t caller_pgid = (tid_t)q.word[0];
    return caller_pgid == fg;
}
static void handle_read(msg_regs_t *m, tid_t from) {
    const vfs_read_req_t *req = (const vfs_read_req_t *)m;
    uint64_t handle = req->handle;
    uint64_t len = req->len > VFS_READ_MAX ? VFS_READ_MAX : req->len;
    vfs_read_reply_t *reply = (vfs_read_reply_t *)m;
    if (!valid_handle(handle)) {
        reply->status = VFS_ERR_BAD_HANDLE;
        return;
    }
    switch ((dev_id_t)handle) {
    case DEV_NULL:
        reply->status = 0;
        break;
    case DEV_ZERO:
        for (uint64_t i = 0; i < len; i++) {
            reply->data[i] = 0;
        }
        reply->status = (int64_t)len;
        break;
    case DEV_RANDOM: {
        if (!rdrand_available) {
            reply->status = VFS_ERR_NOT_SUPPORTED;
            break;
        }
        uint64_t got = 0;
        while (got < len) {
            uint64_t v;
            if (!rdrand64(&v)) {
                break;
            }
            uint8_t *bytes = (uint8_t *)&v;
            for (int j = 0; j < 8 && got < len; j++, got++) {
                reply->data[got] = bytes[j];
            }
        }
        reply->status = (int64_t)got;
        break;
    }
    case DEV_CONSOLE: {
        if (!console_fg_read_allowed(from)) {
            reply->status = 0;
            break;
        }
        if (console_local_head == console_local_tail) {
            uint8_t kbuf[VFS_READ_MAX];
            int got = devfs_kernel_console_read(kbuf, sizeof(kbuf));
            if (got < 0) {
                reply->status = VFS_ERR_NOT_SUPPORTED;
                break;
            }
            for (int i = 0; i < got; i++) {
                uint32_t next = (console_local_head + 1) % CONSOLE_LOCAL_BUF_SIZE;
                if (next == console_local_tail) {
                    break;
                }
                console_local_buf[console_local_head] = kbuf[i];
                console_local_head = next;
            }
        }
        int n = 0;
        while ((uint64_t)n < len && console_local_head != console_local_tail) {
            reply->data[n++] = console_local_buf[console_local_tail];
            console_local_tail = (console_local_tail + 1) % CONSOLE_LOCAL_BUF_SIZE;
        }
        reply->status = n;
        break;
    }
    default:
        reply->status = VFS_ERR_NOT_SUPPORTED;
        break;
    }
}
static void handle_peek(msg_regs_t *m, tid_t from) {
    const vfs_read_req_t *req = (const vfs_read_req_t *)m;
    uint64_t handle = req->handle;
    vfs_read_reply_t *reply = (vfs_read_reply_t *)m;
    if (!valid_handle(handle)) {
        reply->status = VFS_ERR_BAD_HANDLE;
        return;
    }
    switch ((dev_id_t)handle) {
    case DEV_NULL:
    case DEV_ZERO:
    case DEV_RANDOM:
        reply->status = 1;
        break;
    case DEV_CONSOLE: {
        if (!console_fg_read_allowed(from)) {
            reply->status = 0;
            break;
        }
        if (console_local_head == console_local_tail) {
            uint8_t kbuf[VFS_READ_MAX];
            int got = devfs_kernel_console_read(kbuf, sizeof(kbuf));
            if (got > 0) {
                for (int i = 0; i < got; i++) {
                    uint32_t next = (console_local_head + 1) % CONSOLE_LOCAL_BUF_SIZE;
                    if (next == console_local_tail) {
                        break;
                    }
                    console_local_buf[console_local_head] = kbuf[i];
                    console_local_head = next;
                }
            }
        }
        reply->status = console_local_head != console_local_tail ? 1 : 0;
        break;
    }
    default:
        reply->status = 0;
        break;
    }
}
static void handle_write(msg_regs_t *m) {
    const vfs_write_req_t *req = (const vfs_write_req_t *)m;
    uint64_t handle = req->handle;
    uint64_t len = req->len > VFS_WRITE_MAX ? VFS_WRITE_MAX : req->len;
    uint8_t data[VFS_WRITE_MAX];
    for (uint64_t i = 0; i < len; i++) {
        data[i] = req->data[i];
    }
    vfs_write_reply_t *reply = (vfs_write_reply_t *)m;
    if (!valid_handle(handle)) {
        reply->status = VFS_ERR_BAD_HANDLE;
        return;
    }
    switch ((dev_id_t)handle) {
    case DEV_NULL:
    case DEV_ZERO:
    case DEV_RANDOM:
        reply->status = (int64_t)len;
        break;
    case DEV_CONSOLE:
        devfs_kernel_console_write(data, len);
        reply->status = (int64_t)len;
        break;
    }
}
static void handle_close(msg_regs_t *m) {
    const vfs_close_req_t *req = (const vfs_close_req_t *)m;
    uint64_t handle = req->handle;
    vfs_close_reply_t *reply = (vfs_close_reply_t *)m;
    reply->status = valid_handle(handle) ? 0 : VFS_ERR_BAD_HANDLE;
}
static void handle_readdir(msg_regs_t *m) {
    static const char *const names[] = { "console", "null", "zero", "random" };
    const vfs_readdir_req_t *req = (const vfs_readdir_req_t *)m;
    uint64_t want = req->index;
    vfs_readdir_reply_t *reply = (vfs_readdir_reply_t *)m;
    if (want >= sizeof(names) / sizeof(names[0])) {
        reply->status = VFS_ERR_NOT_FOUND;
        return;
    }
    reply->status = 0;
    reply->is_dir = 0;
    int i = 0;
    while (names[want][i]) {
        reply->name[i] = names[want][i];
        i++;
    }
    reply->name[i] = '\0';
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
        reply->ino = 0;
        return;
    }
    dev_id_t id;
    if (resolve_name(name, &id) != 0) {
        reply->status = VFS_ERR_NOT_FOUND;
        return;
    }
    reply->status = 0;
    reply->size = 0;
    reply->is_dir = 0;
    reply->ino = (uint64_t)id + 1;
}
static void handle_fstat(msg_regs_t *m) {
    const vfs_fstat_req_t *req = (const vfs_fstat_req_t *)m;
    uint64_t handle = req->handle;
    vfs_stat_reply_t *reply = (vfs_stat_reply_t *)m;
    if (!valid_handle(handle)) {
        reply->status = VFS_ERR_BAD_HANDLE;
        return;
    }
    reply->status = 0;
    reply->size = 0;
    reply->is_dir = 0;
    reply->ino = handle + 1;
}
void _start(void) {
    check_rdrand();
    msg_regs_t m;
    tid_t from;
    for (;;) {
        ipc_recv(IPC_TID_ANY, &m, &from);
        switch (m.word[0]) {
        case VFS_OP_OPEN:
            handle_open(&m);
            break;
        case VFS_OP_READ:
            handle_read(&m, from);
            break;
        case VFS_OP_PEEK:
            handle_peek(&m, from);
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
