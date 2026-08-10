#include "robu/types.h"
#include "robu/uipc.h"
#include "robu/ipc.h"
#include "robu/kinfo.h"
#include "robu/vfs.h"
#define SYSFS_MAX_HANDLES 16
#define SYSFS_CONTENT_MAX 96
typedef struct {
    int in_use;
    uint64_t offset;
    uint64_t len;
    char content[SYSFS_CONTENT_MAX];
} sysfs_handle_t;
static sysfs_handle_t handles[SYSFS_MAX_HANDLES];
static int alloc_handle(void) {
    for (int i = 0; i < SYSFS_MAX_HANDLES; i++) {
        if (!handles[i].in_use) {
            return i;
        }
    }
    return -1;
}
static int valid_handle(uint64_t h) {
    return h < SYSFS_MAX_HANDLES && handles[h].in_use;
}
static int path_eq(const char *a, const char *b) {
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
static void format_meminfo(sysfs_handle_t *hd) {
    msg_regs_t q = (msg_regs_t){0};
    q.word[0] = 0;
    robu_ipc_raw(0, 0, IPC_FLAG_SYS_INFO, &q, NULL);
    int pos = 0;
    pos = append_str(hd->content, pos, SYSFS_CONTENT_MAX, "total_frames=");
    pos = append_uint(hd->content, pos, SYSFS_CONTENT_MAX, q.word[0]);
    pos = append_str(hd->content, pos, SYSFS_CONTENT_MAX, " free_frames=");
    pos = append_uint(hd->content, pos, SYSFS_CONTENT_MAX, q.word[1]);
    pos = append_str(hd->content, pos, SYSFS_CONTENT_MAX, " alloc_calls=");
    pos = append_uint(hd->content, pos, SYSFS_CONTENT_MAX, q.word[2]);
    pos = append_str(hd->content, pos, SYSFS_CONTENT_MAX, " free_calls=");
    pos = append_uint(hd->content, pos, SYSFS_CONTENT_MAX, q.word[3]);
    if (pos < SYSFS_CONTENT_MAX) {
        hd->content[pos++] = '\n';
    }
    hd->len = (uint64_t)pos;
}
static void format_schedstats(sysfs_handle_t *hd) {
    msg_regs_t q = (msg_regs_t){0};
    q.word[0] = 1;
    robu_ipc_raw(0, 0, IPC_FLAG_SYS_INFO, &q, NULL);
    int pos = 0;
    pos = append_str(hd->content, pos, SYSFS_CONTENT_MAX, "full_scheds=");
    pos = append_uint(hd->content, pos, SYSFS_CONTENT_MAX, q.word[0]);
    pos = append_str(hd->content, pos, SYSFS_CONTENT_MAX, " preempts=");
    pos = append_uint(hd->content, pos, SYSFS_CONTENT_MAX, q.word[1]);
    pos = append_str(hd->content, pos, SYSFS_CONTENT_MAX, " ipc_msgs=");
    pos = append_uint(hd->content, pos, SYSFS_CONTENT_MAX, q.word[2]);
    pos = append_str(hd->content, pos, SYSFS_CONTENT_MAX, " ticks=");
    pos = append_uint(hd->content, pos, SYSFS_CONTENT_MAX, q.word[3]);
    pos = append_str(hd->content, pos, SYSFS_CONTENT_MAX, " timer_traps=");
    pos = append_uint(hd->content, pos, SYSFS_CONTENT_MAX, q.word[4]);
    pos = append_str(hd->content, pos, SYSFS_CONTENT_MAX, " kicks_sent=");
    pos = append_uint(hd->content, pos, SYSFS_CONTENT_MAX, q.word[5]);
    if (pos < SYSFS_CONTENT_MAX) {
        hd->content[pos++] = '\n';
    }
    hd->len = (uint64_t)pos;
}
static void format_uptime(sysfs_handle_t *hd) {
    const volatile kinfo_page_t *k = (const volatile kinfo_page_t *)KINFO_VA;
    uint64_t ticks = kinfo_read_ticks(k);
    int pos = 0;
    pos = append_str(hd->content, pos, SYSFS_CONTENT_MAX, "uptime_ticks=");
    pos = append_uint(hd->content, pos, SYSFS_CONTENT_MAX, ticks);
    pos = append_str(hd->content, pos, SYSFS_CONTENT_MAX, " hz=");
    pos = append_uint(hd->content, pos, SYSFS_CONTENT_MAX, k->clock_hz);
    if (pos < SYSFS_CONTENT_MAX) {
        hd->content[pos++] = '\n';
    }
    hd->len = (uint64_t)pos;
}

static int fill_content(const char *name, sysfs_handle_t *hd) {
    if (path_eq(name, "meminfo")) {
        format_meminfo(hd);
        return 1;
    }
    if (path_eq(name, "schedstats")) {
        format_schedstats(hd);
        return 2;
    }
    if (path_eq(name, "uptime")) {
        format_uptime(hd);
        return 3;
    }
    return 0;
}
static void handle_open(msg_regs_t *m) {
    const vfs_open_req_t *req = (const vfs_open_req_t *)m;
    char name[VFS_PATH_MAX];
    for (int i = 0; i < VFS_PATH_MAX; i++) {
        name[i] = req->name[i];
    }
    vfs_open_reply_t *reply = (vfs_open_reply_t *)m;
    int hidx = alloc_handle();
    if (hidx < 0) {
        reply->status = VFS_ERR_NO_SPACE;
        return;
    }
    sysfs_handle_t *hd = &handles[hidx];
    hd->offset = 0;
    if (fill_content(name, hd) == 0) {
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
    sysfs_handle_t *hd = &handles[h];
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
    char name[VFS_PATH_MAX];
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
    sysfs_handle_t scratch = {0};
    int id = fill_content(name, &scratch);
    if (id == 0) {
        reply->status = VFS_ERR_NOT_FOUND;
        return;
    }
    reply->status = 0;
    reply->size = scratch.len;
    reply->is_dir = 0;
    reply->ino = (uint64_t)id;
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
static void handle_readdir(msg_regs_t *m) {
    static const char *const names[] = { "meminfo", "schedstats", "uptime" };
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
