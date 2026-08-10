#include "robu/types.h"
#include "robu/uipc.h"
#include "robu/ipc.h"
#include "robu/vfs.h"
#define PROCFS_MAX_HANDLES 16
#define PROCFS_CONTENT_MAX 96
typedef struct {
    int in_use;
    uint64_t offset;
    uint64_t len;
    char content[PROCFS_CONTENT_MAX];
} procfs_handle_t;
static procfs_handle_t handles[PROCFS_MAX_HANDLES];
static int alloc_handle(void) {
    for (int i = 0; i < PROCFS_MAX_HANDLES; i++) {
        if (!handles[i].in_use) {
            return i;
        }
    }
    return -1;
}
static int valid_handle(uint64_t h) {
    return h < PROCFS_MAX_HANDLES && handles[h].in_use;
}
static uint32_t parse_tid_status_path(const char *path) {
    uint32_t tid = 0;
    int ndigits = 0;
    const char *p = path;
    while (*p >= '0' && *p <= '9') {
        tid = tid * 10 + (uint32_t)(*p - '0');
        p++;
        ndigits++;
    }
    if (ndigits == 0) {
        return 0;
    }
    static const char suffix[] = "/status";
    for (int i = 0; suffix[i]; i++) {
        if (p[i] != suffix[i]) {
            return 0;
        }
    }
    if (p[sizeof(suffix) - 1] != '\0') {
        return 0;
    }
    return tid;
}
static int append_str(char *buf, int pos, int max, const char *s) {
    while (*s && pos < max) {
        buf[pos++] = *s++;
    }
    return pos;
}
static int append_uint(char *buf, int pos, int max, uint64_t v) {
    char digits[20];
    int nd = 0;
    if (v == 0) {
        digits[nd++] = '0';
    } else {
        while (v > 0 && nd < 20) {
            digits[nd++] = (char)('0' + (v % 10));
            v /= 10;
        }
    }
    while (nd > 0 && pos < max) {
        buf[pos++] = digits[--nd];
    }
    return pos;
}
static int append_int(char *buf, int pos, int max, int64_t v) {
    if (v < 0) {
        if (pos < max) {
            buf[pos++] = '-';
        }
        return append_uint(buf, pos, max, (uint64_t)(-v));
    }
    return append_uint(buf, pos, max, (uint64_t)v);
}

static int fill_content(uint32_t tid, procfs_handle_t *hd) {
    msg_regs_t q = (msg_regs_t){0};
    q.word[0] = tid;
    int64_t rc = robu_ipc_raw(0, 0, IPC_FLAG_THREAD_INFO, &q, NULL);
    if (rc != IPC_ERR_NONE) {
        return -1;
    }
    uint64_t state = q.word[0];
    uint64_t prio = q.word[1];
    int64_t exit_status = (int64_t)q.word[2];
    uint64_t parent_tid = q.word[3];
    uint64_t w13 = q.word[4], w14 = q.word[5];
    char name[17];
    int ni = 0;
    for (int i = 0; i < 8; i++) {
        uint8_t c = (uint8_t)(w13 >> (8 * i));
        if (!c) {
            break;
        }
        name[ni++] = (char)c;
    }
    if (ni == 8) {
        for (int i = 0; i < 8; i++) {
            uint8_t c = (uint8_t)(w14 >> (8 * i));
            if (!c) {
                break;
            }
            name[ni++] = (char)c;
        }
    }
    name[ni] = '\0';
    hd->offset = 0;
    int pos = 0;
    pos = append_str(hd->content, pos, PROCFS_CONTENT_MAX, "tid=");
    pos = append_uint(hd->content, pos, PROCFS_CONTENT_MAX, tid);
    pos = append_str(hd->content, pos, PROCFS_CONTENT_MAX, " name=");
    pos = append_str(hd->content, pos, PROCFS_CONTENT_MAX, name);
    pos = append_str(hd->content, pos, PROCFS_CONTENT_MAX, " state=");
    pos = append_uint(hd->content, pos, PROCFS_CONTENT_MAX, state);
    pos = append_str(hd->content, pos, PROCFS_CONTENT_MAX, " prio=");
    pos = append_uint(hd->content, pos, PROCFS_CONTENT_MAX, prio);
    pos = append_str(hd->content, pos, PROCFS_CONTENT_MAX, " parent=");
    pos = append_uint(hd->content, pos, PROCFS_CONTENT_MAX, parent_tid);
    pos = append_str(hd->content, pos, PROCFS_CONTENT_MAX, " exit=");
    pos = append_int(hd->content, pos, PROCFS_CONTENT_MAX, exit_status);
    if (pos < PROCFS_CONTENT_MAX) {
        hd->content[pos++] = '\n';
    }
    hd->len = (uint64_t)pos;
    return 0;
}
static void handle_open(msg_regs_t *m) {
    const vfs_open_req_t *req = (const vfs_open_req_t *)m;
    char path[VFS_PATH_MAX];
    for (int i = 0; i < VFS_PATH_MAX; i++) {
        path[i] = req->name[i];
    }
    vfs_open_reply_t *reply = (vfs_open_reply_t *)m;
    uint32_t tid = parse_tid_status_path(path);
    if (tid == 0) {
        reply->status = VFS_ERR_NOT_FOUND;
        return;
    }
    int hidx = alloc_handle();
    if (hidx < 0) {
        reply->status = VFS_ERR_NO_SPACE;
        return;
    }
    procfs_handle_t *hd = &handles[hidx];
    if (fill_content(tid, hd) != 0) {
        reply->status = VFS_ERR_NOT_FOUND;
        return;
    }
    hd->in_use = 1;
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
    procfs_handle_t *hd = &handles[h];
    uint64_t avail = hd->len > hd->offset ? hd->len - hd->offset : 0;
    uint64_t n = len < avail ? len : avail;
    for (uint64_t i = 0; i < n; i++) {
        reply->data[i] = (uint8_t)hd->content[hd->offset + i];
    }
    hd->offset += n;
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
    const vfs_stat_req_t *req = (const vfs_stat_req_t *)m;
    char path[VFS_PATH_MAX];
    for (int i = 0; i < VFS_PATH_MAX; i++) {
        path[i] = req->name[i];
    }
    vfs_stat_reply_t *reply = (vfs_stat_reply_t *)m;
    if (path[0] == '\0') {

        reply->status = 0;
        reply->size = 0;
        reply->is_dir = 1;
        reply->ino = 0;
        return;
    }
    uint32_t tid = parse_tid_status_path(path);
    if (tid == 0) {
        reply->status = VFS_ERR_NOT_FOUND;
        return;
    }
    procfs_handle_t scratch = {0};
    if (fill_content(tid, &scratch) != 0) {
        reply->status = VFS_ERR_NOT_FOUND;
        return;
    }
    reply->status = 0;
    reply->size = scratch.len;
    reply->is_dir = 0;
    reply->ino = (uint64_t)tid;
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
    reply->size = handles[h].len;
    reply->is_dir = 0;
    reply->ino = h + 1;
}

#define PROCFS_MAX_SCAN_TID 64
static void handle_readdir(msg_regs_t *m) {
    const vfs_readdir_req_t *req = (const vfs_readdir_req_t *)m;
    uint64_t want = req->index;
    vfs_readdir_reply_t *reply = (vfs_readdir_reply_t *)m;
    uint64_t seen = 0;
    for (uint32_t tid = 1; tid < PROCFS_MAX_SCAN_TID; tid++) {
        msg_regs_t q = (msg_regs_t){0};
        q.word[0] = tid;
        int64_t rc = robu_ipc_raw(0, 0, IPC_FLAG_THREAD_INFO, &q, NULL);
        if (rc != IPC_ERR_NONE) {
            continue;
        }
        if (seen == want) {
            reply->status = 0;
            reply->is_dir = 0;
            int pos = append_uint(reply->name, 0, VFS_NAME_MAX, tid);
            reply->name[pos] = '\0';
            return;
        }
        seen++;
    }
    reply->status = VFS_ERR_NOT_FOUND;
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
