#include "robu/types.h"
#include "robu/uipc.h"
#include "robu/ipc.h"
#include "robu/kinfo.h"
#include "robu/vfs.h"

#define PROCFS_MAX_HANDLES 16
#define PROCFS_CONTENT_MAX 2048
#define PROCFS_MAX_SCAN_TID 64
#define PROCFS_ROOT_INO 0
#define PROCFS_SELF_INO (~0ULL)

typedef enum {
    PROCFS_FILE_NONE = 0,
    PROCFS_FILE_CPUINFO,
    PROCFS_FILE_FILESYSTEMS,
    PROCFS_FILE_MEMINFO,
    PROCFS_FILE_MOUNTS,
    PROCFS_FILE_STAT,
    PROCFS_FILE_UPTIME,
    PROCFS_FILE_VERSION,
    PROCFS_FILE_CMDLINE,
    PROCFS_FILE_COMM,
    PROCFS_FILE_PROCESS_STAT,
    PROCFS_FILE_STATUS,
} procfs_file_t;

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

static int path_prefix(const char *path, const char *prefix) {
    for (int i = 0; prefix[i]; i++) {
        if (path[i] != prefix[i]) {
            return 0;
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
        while (v > 0 && nd < (int)sizeof(digits)) {
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
        return append_uint(buf, pos, max, (uint64_t)(-(v + 1)) + 1);
    }
    return append_uint(buf, pos, max, (uint64_t)v);
}

static int append_byte(char *buf, int pos, int max, char c) {
    if (pos < max) {
        buf[pos++] = c;
    }
    return pos;
}

static int thread_info(uint32_t tid, msg_regs_t *out) {
    msg_regs_t q = (msg_regs_t){0};
    q.word[0] = tid;
    if (robu_ipc_raw(0, 0, IPC_FLAG_THREAD_INFO, &q, NULL) != IPC_ERR_NONE) {
        return -1;
    }
    *out = q;
    return 0;
}

static uint32_t thread_pgid(uint32_t tid) {
    msg_regs_t q = (msg_regs_t){0};
    q.word[0] = SYS_INFO_CAT_GETPGID;
    q.word[1] = tid;
    if (robu_ipc_raw(0, 0, IPC_FLAG_SYS_INFO, &q, NULL) != IPC_ERR_NONE) {
        return 0;
    }
    return (uint32_t)q.word[0];
}

static int sys_info(uint64_t category, msg_regs_t *out) {
    msg_regs_t q = (msg_regs_t){0};
    q.word[0] = category;
    if (robu_ipc_raw(0, 0, IPC_FLAG_SYS_INFO, &q, NULL) != IPC_ERR_NONE) {
        return -1;
    }
    *out = q;
    return 0;
}

static void thread_name(const msg_regs_t *q, char name[17]) {
    int ni = 0;
    for (int i = 0; i < 8; i++) {
        uint8_t c = (uint8_t)(q->word[4] >> (8 * i));
        if (!c) {
            break;
        }
        name[ni++] = (char)c;
    }
    if (ni == 8) {
        for (int i = 0; i < 8; i++) {
            uint8_t c = (uint8_t)(q->word[5] >> (8 * i));
            if (!c) {
                break;
            }
            name[ni++] = (char)c;
        }
    }
    name[ni] = '\0';
}

static char state_char(uint64_t state) {
    if (state == THREAD_STATE_RUNNING || state == THREAD_STATE_READY) {
        return 'R';
    }
    if (state == THREAD_STATE_ZOMBIE) {
        return 'Z';
    }
    if (state == THREAD_STATE_DEAD) {
        return 'X';
    }
    return 'S';
}

static const char *state_name(uint64_t state) {
    if (state == THREAD_STATE_RUNNING || state == THREAD_STATE_READY) {
        return "running";
    }
    if (state == THREAD_STATE_ZOMBIE) {
        return "zombie";
    }
    if (state == THREAD_STATE_DEAD) {
        return "dead";
    }
    return "sleeping";
}

static int root_file_kind(const char *path) {
    if (path_eq(path, "cpuinfo")) {
        return PROCFS_FILE_CPUINFO;
    }
    if (path_eq(path, "filesystems")) {
        return PROCFS_FILE_FILESYSTEMS;
    }
    if (path_eq(path, "meminfo")) {
        return PROCFS_FILE_MEMINFO;
    }
    if (path_eq(path, "mounts")) {
        return PROCFS_FILE_MOUNTS;
    }
    if (path_eq(path, "stat")) {
        return PROCFS_FILE_STAT;
    }
    if (path_eq(path, "uptime")) {
        return PROCFS_FILE_UPTIME;
    }
    if (path_eq(path, "version")) {
        return PROCFS_FILE_VERSION;
    }
    return PROCFS_FILE_NONE;
}

static int process_file_kind(const char *name) {
    if (path_eq(name, "cmdline")) {
        return PROCFS_FILE_CMDLINE;
    }
    if (path_eq(name, "comm")) {
        return PROCFS_FILE_COMM;
    }
    if (path_eq(name, "stat")) {
        return PROCFS_FILE_PROCESS_STAT;
    }
    if (path_eq(name, "status")) {
        return PROCFS_FILE_STATUS;
    }
    return PROCFS_FILE_NONE;
}

static int parse_numeric_process_path(const char *path, uint32_t *tid_out,
                                      const char **leaf_out) {
    uint32_t tid = 0;
    int ndigits = 0;
    const char *p = path;
    while (*p >= '0' && *p <= '9') {
        uint32_t digit = (uint32_t)(*p - '0');
        if (tid > (PROCFS_MAX_SCAN_TID - digit) / 10) {
            return -1;
        }
        tid = tid * 10 + digit;
        p++;
        ndigits++;
    }
    if (ndigits == 0 || tid == 0 || tid >= PROCFS_MAX_SCAN_TID) {
        return -1;
    }
    if (*p == '\0') {
        *tid_out = tid;
        *leaf_out = NULL;
        return 0;
    }
    if (*p != '/' || p[1] == '\0') {
        return -1;
    }
    *tid_out = tid;
    *leaf_out = p + 1;
    return 0;
}

static int resolve_process_path(const char *path, tid_t from, uint32_t *tid_out,
                                const char **leaf_out) {
    if (path_eq(path, "self")) {
        if (from == 0 || from >= PROCFS_MAX_SCAN_TID) {
            return -1;
        }
        *tid_out = from;
        *leaf_out = NULL;
        return 0;
    }
    if (path_prefix(path, "self/")) {
        if (from == 0 || from >= PROCFS_MAX_SCAN_TID || path[5] == '\0') {
            return -1;
        }
        *tid_out = from;
        *leaf_out = path + 5;
        return 0;
    }
    return parse_numeric_process_path(path, tid_out, leaf_out);
}

static void finish_content(procfs_handle_t *hd, int pos) {
    hd->offset = 0;
    hd->len = (uint64_t)pos;
}

static void format_cpuinfo(procfs_handle_t *hd) {
    const volatile kinfo_page_t *k = kinfo_user();
    uint32_t cpus = k->cpu_count;
    int pos = 0;
    for (uint32_t cpu = 0; cpu < cpus; cpu++) {
        pos = append_str(hd->content, pos, PROCFS_CONTENT_MAX, "processor\t: ");
        pos = append_uint(hd->content, pos, PROCFS_CONTENT_MAX, cpu);
        pos = append_str(hd->content, pos, PROCFS_CONTENT_MAX, "\nvendor_id\t: Robu\nmodel name\t: Robu x86_64\n\n");
    }
    finish_content(hd, pos);
}

static void format_filesystems(procfs_handle_t *hd) {
    int pos = 0;
    pos = append_str(hd->content, pos, PROCFS_CONTENT_MAX,
                     "nodev\tproc\nnodev\tsysfs\nnodev\tdevtmpfs\nnodev\ttmpfs\next2\nvfat\ndiskfs\n");
    finish_content(hd, pos);
}

static void format_meminfo(procfs_handle_t *hd) {
    msg_regs_t q = (msg_regs_t){0};
    sys_info(0, &q);
    uint64_t total_kib = q.word[0] * 4;
    uint64_t free_kib = q.word[1] * 4;
    int pos = 0;
    pos = append_str(hd->content, pos, PROCFS_CONTENT_MAX, "MemTotal:       ");
    pos = append_uint(hd->content, pos, PROCFS_CONTENT_MAX, total_kib);
    pos = append_str(hd->content, pos, PROCFS_CONTENT_MAX, " kB\nMemFree:        ");
    pos = append_uint(hd->content, pos, PROCFS_CONTENT_MAX, free_kib);
    pos = append_str(hd->content, pos, PROCFS_CONTENT_MAX, " kB\nMemAvailable:   ");
    pos = append_uint(hd->content, pos, PROCFS_CONTENT_MAX, free_kib);
    pos = append_str(hd->content, pos, PROCFS_CONTENT_MAX, " kB\nCached:         0 kB\nSReclaimable:   0 kB\n");
    finish_content(hd, pos);
}

static const char *mount_type(const volatile kinfo_page_t *k, uint32_t tid) {
    if (tid == k->procfs_tid) {
        return "proc";
    }
    if (tid == k->sysfs_tid) {
        return "sysfs";
    }
    if (tid == k->devfs_tid) {
        return "devtmpfs";
    }
    if (tid == k->ramfs_tid) {
        return "tmpfs";
    }
    if (tid == k->ext2fs_tid) {
        return "ext2";
    }
    if (tid == k->diskfs_tid) {
        return "diskfs";
    }
    return "robu";
}

static void append_mount_path(char *buf, int *pos, const char *path) {
    int len = 0;
    while (len < MOUNT_PREFIX_MAX && path[len]) {
        len++;
    }
    if (len > 1 && path[len - 1] == '/') {
        len--;
    }
    for (int i = 0; i < len; i++) {
        *pos = append_byte(buf, *pos, PROCFS_CONTENT_MAX, path[i]);
    }
}

static void format_mounts(procfs_handle_t *hd) {
    const volatile kinfo_page_t *k = kinfo_user();
    int pos = 0;
    for (int i = 0; i < MOUNT_TABLE_MAX; i++) {
        mount_entry_t entry;
        int retry = 0;
        do {
            uint32_t seq0 = k->mount_seq;
            asm volatile("" ::: "memory");
            entry = k->mounts[i];
            asm volatile("" ::: "memory");
            uint32_t seq1 = k->mount_seq;
            retry = seq0 != seq1 || (seq0 & 1u);
        } while (retry);
        if (!entry.in_use) {
            continue;
        }
        msg_regs_t q;
        char name[17] = "robu";
        if (thread_info(entry.owner_tid, &q) == 0) {
            thread_name(&q, name);
        }
        pos = append_str(hd->content, pos, PROCFS_CONTENT_MAX, name);
        pos = append_byte(hd->content, pos, PROCFS_CONTENT_MAX, ' ');
        append_mount_path(hd->content, &pos, entry.prefix);
        pos = append_str(hd->content, pos, PROCFS_CONTENT_MAX, " ");
        pos = append_str(hd->content, pos, PROCFS_CONTENT_MAX, mount_type(k, entry.owner_tid));
        pos = append_str(hd->content, pos, PROCFS_CONTENT_MAX, " rw 0 0\n");
    }
    finish_content(hd, pos);
}

static void format_stat(procfs_handle_t *hd) {
    msg_regs_t stats = (msg_regs_t){0};
    sys_info(1, &stats);
    uint64_t total = 0;
    uint64_t runnable = 0;
    uint64_t blocked = 0;
    for (uint32_t tid = 1; tid < PROCFS_MAX_SCAN_TID; tid++) {
        msg_regs_t q;
        if (thread_info(tid, &q) != 0) {
            continue;
        }
        total++;
        if (q.word[0] == THREAD_STATE_RUNNING || q.word[0] == THREAD_STATE_READY) {
            runnable++;
        } else if (q.word[0] != THREAD_STATE_ZOMBIE) {
            blocked++;
        }
    }
    int pos = 0;
    pos = append_str(hd->content, pos, PROCFS_CONTENT_MAX, "cpu 0 0 0 0 0 0 0 0\nctxt ");
    pos = append_uint(hd->content, pos, PROCFS_CONTENT_MAX, stats.word[0]);
    pos = append_str(hd->content, pos, PROCFS_CONTENT_MAX, "\nprocesses ");
    pos = append_uint(hd->content, pos, PROCFS_CONTENT_MAX, total);
    pos = append_str(hd->content, pos, PROCFS_CONTENT_MAX, "\nprocs_running ");
    pos = append_uint(hd->content, pos, PROCFS_CONTENT_MAX, runnable);
    pos = append_str(hd->content, pos, PROCFS_CONTENT_MAX, "\nprocs_blocked ");
    pos = append_uint(hd->content, pos, PROCFS_CONTENT_MAX, blocked);
    pos = append_byte(hd->content, pos, PROCFS_CONTENT_MAX, '\n');
    finish_content(hd, pos);
}

static void format_uptime(procfs_handle_t *hd) {
    const volatile kinfo_page_t *k = kinfo_user();
    uint64_t hz = k->clock_hz ? k->clock_hz : 1;
    uint64_t ticks = kinfo_read_ticks(k);
    uint64_t hundredths = ((ticks % hz) * 100) / hz;
    int pos = 0;
    pos = append_uint(hd->content, pos, PROCFS_CONTENT_MAX, ticks / hz);
    pos = append_byte(hd->content, pos, PROCFS_CONTENT_MAX, '.');
    if (hundredths < 10) {
        pos = append_byte(hd->content, pos, PROCFS_CONTENT_MAX, '0');
    }
    pos = append_uint(hd->content, pos, PROCFS_CONTENT_MAX, hundredths);
    pos = append_str(hd->content, pos, PROCFS_CONTENT_MAX, " 0.00\n");
    finish_content(hd, pos);
}

static void format_version(procfs_handle_t *hd) {
    int pos = append_str(hd->content, 0, PROCFS_CONTENT_MAX, "Robu Kernel 0.9 x86_64\n");
    finish_content(hd, pos);
}

static int format_global_file(procfs_file_t kind, procfs_handle_t *hd) {
    if (kind == PROCFS_FILE_CPUINFO) {
        format_cpuinfo(hd);
    } else if (kind == PROCFS_FILE_FILESYSTEMS) {
        format_filesystems(hd);
    } else if (kind == PROCFS_FILE_MEMINFO) {
        format_meminfo(hd);
    } else if (kind == PROCFS_FILE_MOUNTS) {
        format_mounts(hd);
    } else if (kind == PROCFS_FILE_STAT) {
        format_stat(hd);
    } else if (kind == PROCFS_FILE_UPTIME) {
        format_uptime(hd);
    } else if (kind == PROCFS_FILE_VERSION) {
        format_version(hd);
    } else {
        return -1;
    }
    return 0;
}

static int format_process_file(uint32_t tid, procfs_file_t kind, procfs_handle_t *hd) {
    msg_regs_t q;
    if (thread_info(tid, &q) != 0) {
        return -1;
    }
    uint64_t state = q.word[0];
    uint64_t prio = q.word[1];
    int64_t exit_status = (int64_t)q.word[2];
    uint64_t parent_tid = q.word[3];
    char name[17];
    thread_name(&q, name);
    int pos = 0;
    if (kind == PROCFS_FILE_CMDLINE) {
        pos = append_str(hd->content, pos, PROCFS_CONTENT_MAX, name);
        pos = append_byte(hd->content, pos, PROCFS_CONTENT_MAX, '\0');
    } else if (kind == PROCFS_FILE_COMM) {
        pos = append_str(hd->content, pos, PROCFS_CONTENT_MAX, name);
        pos = append_byte(hd->content, pos, PROCFS_CONTENT_MAX, '\n');
    } else if (kind == PROCFS_FILE_STATUS) {
        pos = append_str(hd->content, pos, PROCFS_CONTENT_MAX, "Name:\t");
        pos = append_str(hd->content, pos, PROCFS_CONTENT_MAX, name);
        pos = append_str(hd->content, pos, PROCFS_CONTENT_MAX, "\nState:\t");
        pos = append_byte(hd->content, pos, PROCFS_CONTENT_MAX, state_char(state));
        pos = append_str(hd->content, pos, PROCFS_CONTENT_MAX, " (");
        pos = append_str(hd->content, pos, PROCFS_CONTENT_MAX, state_name(state));
        pos = append_str(hd->content, pos, PROCFS_CONTENT_MAX, ")\nTgid:\t");
        pos = append_uint(hd->content, pos, PROCFS_CONTENT_MAX, tid);
        pos = append_str(hd->content, pos, PROCFS_CONTENT_MAX, "\nPid:\t");
        pos = append_uint(hd->content, pos, PROCFS_CONTENT_MAX, tid);
        pos = append_str(hd->content, pos, PROCFS_CONTENT_MAX, "\nPPid:\t");
        pos = append_uint(hd->content, pos, PROCFS_CONTENT_MAX, parent_tid);
        pos = append_str(hd->content, pos, PROCFS_CONTENT_MAX, "\nPriority:\t");
        pos = append_uint(hd->content, pos, PROCFS_CONTENT_MAX, prio);
        pos = append_str(hd->content, pos, PROCFS_CONTENT_MAX, "\nExitCode:\t");
        pos = append_int(hd->content, pos, PROCFS_CONTENT_MAX, exit_status);
        pos = append_byte(hd->content, pos, PROCFS_CONTENT_MAX, '\n');
    } else if (kind == PROCFS_FILE_PROCESS_STAT) {
        uint32_t pgid = thread_pgid(tid);
        pos = append_uint(hd->content, pos, PROCFS_CONTENT_MAX, tid);
        pos = append_str(hd->content, pos, PROCFS_CONTENT_MAX, " (");
        pos = append_str(hd->content, pos, PROCFS_CONTENT_MAX, name);
        pos = append_str(hd->content, pos, PROCFS_CONTENT_MAX, ") ");
        pos = append_byte(hd->content, pos, PROCFS_CONTENT_MAX, state_char(state));
        pos = append_byte(hd->content, pos, PROCFS_CONTENT_MAX, ' ');
        pos = append_uint(hd->content, pos, PROCFS_CONTENT_MAX, parent_tid);
        pos = append_byte(hd->content, pos, PROCFS_CONTENT_MAX, ' ');
        pos = append_uint(hd->content, pos, PROCFS_CONTENT_MAX, pgid);
        pos = append_str(hd->content, pos, PROCFS_CONTENT_MAX,
                         " 0 0 0 0 0 0 0 0 0 0 0 0 ");
        pos = append_uint(hd->content, pos, PROCFS_CONTENT_MAX, prio);
        pos = append_str(hd->content, pos, PROCFS_CONTENT_MAX,
                         " 0 1 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0\n");
    } else {
        return -1;
    }
    finish_content(hd, pos);
    return 0;
}

static int fill_content(const char *path, tid_t from, procfs_handle_t *hd,
                        procfs_file_t *kind_out, uint32_t *tid_out) {
    procfs_file_t kind = (procfs_file_t)root_file_kind(path);
    if (kind != PROCFS_FILE_NONE) {
        if (format_global_file(kind, hd) != 0) {
            return -1;
        }
        *kind_out = kind;
        *tid_out = 0;
        return 0;
    }
    uint32_t tid;
    const char *leaf;
    if (resolve_process_path(path, from, &tid, &leaf) != 0 || leaf == NULL) {
        return -1;
    }
    kind = (procfs_file_t)process_file_kind(leaf);
    if (kind == PROCFS_FILE_NONE || format_process_file(tid, kind, hd) != 0) {
        return -1;
    }
    *kind_out = kind;
    *tid_out = tid;
    return 0;
}

static uint64_t file_ino(procfs_file_t kind, uint32_t tid) {
    if (tid == 0) {
        return 0x1000ULL + (uint64_t)kind;
    }
    return 0x2000ULL + (uint64_t)tid * 16 + (uint64_t)kind;
}

static void copy_reply_name(vfs_readdir_reply_t *reply, const char *name) {
    int i = 0;
    while (i < VFS_NAME_MAX - 1 && name[i]) {
        reply->name[i] = name[i];
        i++;
    }
    reply->name[i] = '\0';
}

static void handle_caps(msg_regs_t *m) {
    vfs_caps_reply_t *reply = (vfs_caps_reply_t *)m;
    reply->status = 0;
    reply->abi = ((uint64_t)ROBU_VFS_ABI_MAJOR << 32) | ROBU_VFS_ABI_MINOR;
    reply->features = ROBU_VFS_FEATURE_OPEN |
                      ROBU_VFS_FEATURE_READ |
                      ROBU_VFS_FEATURE_STAT |
                      ROBU_VFS_FEATURE_READDIR |
                      ROBU_VFS_FEATURE_LINUX_VFS;
}

static void handle_open(msg_regs_t *m, tid_t from) {
    const vfs_open_req_t *req = (const vfs_open_req_t *)m;
    char path[VFS_PATH_MAX];
    for (int i = 0; i < VFS_PATH_MAX; i++) {
        path[i] = req->name[i];
    }
    vfs_open_reply_t *reply = (vfs_open_reply_t *)m;
    int hidx = alloc_handle();
    if (hidx < 0) {
        reply->status = VFS_ERR_NO_SPACE;
        return;
    }
    procfs_handle_t *hd = &handles[hidx];
    procfs_file_t kind;
    uint32_t tid;
    if (fill_content(path, from, hd, &kind, &tid) != 0) {
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
    if (!valid_handle(req->handle)) {
        reply->status = VFS_ERR_BAD_HANDLE;
        return;
    }
    handles[req->handle].in_use = 0;
    reply->status = 0;
}

static void handle_stat(msg_regs_t *m, tid_t from) {
    const vfs_stat_req_t *req = (const vfs_stat_req_t *)m;
    char path[VFS_PATH_MAX];
    for (int i = 0; i < VFS_PATH_MAX; i++) {
        path[i] = req->name[i];
    }
    vfs_stat_reply_t *reply = (vfs_stat_reply_t *)m;
    if (path[0] == '\0') {
        reply->status = 0;
        reply->size = 0;
        reply->is_dir = VFS_NODE_DIR;
        reply->ino = PROCFS_ROOT_INO;
        return;
    }
    procfs_file_t root_kind = (procfs_file_t)root_file_kind(path);
    if (root_kind != PROCFS_FILE_NONE) {
        procfs_handle_t scratch;
        if (format_global_file(root_kind, &scratch) != 0) {
            reply->status = VFS_ERR_NOT_FOUND;
            return;
        }
        reply->status = 0;
        reply->size = scratch.len;
        reply->is_dir = VFS_NODE_REG;
        reply->ino = file_ino(root_kind, 0);
        return;
    }
    uint32_t tid;
    const char *leaf;
    if (resolve_process_path(path, from, &tid, &leaf) != 0) {
        reply->status = VFS_ERR_NOT_FOUND;
        return;
    }
    msg_regs_t q;
    if (thread_info(tid, &q) != 0) {
        reply->status = VFS_ERR_NOT_FOUND;
        return;
    }
    if (leaf == NULL) {
        reply->status = 0;
        reply->size = 0;
        reply->is_dir = VFS_NODE_DIR;
        reply->ino = path_eq(path, "self") ? PROCFS_SELF_INO : (uint64_t)tid;
        return;
    }
    procfs_file_t kind = (procfs_file_t)process_file_kind(leaf);
    procfs_handle_t scratch;
    if (kind == PROCFS_FILE_NONE || format_process_file(tid, kind, &scratch) != 0) {
        reply->status = VFS_ERR_NOT_FOUND;
        return;
    }
    reply->status = 0;
    reply->size = scratch.len;
    reply->is_dir = VFS_NODE_REG;
    reply->ino = file_ino(kind, tid);
}

static void handle_fstat(msg_regs_t *m) {
    const vfs_fstat_req_t *req = (const vfs_fstat_req_t *)m;
    vfs_stat_reply_t *reply = (vfs_stat_reply_t *)m;
    if (!valid_handle(req->handle)) {
        reply->status = VFS_ERR_BAD_HANDLE;
        return;
    }
    reply->status = 0;
    reply->size = handles[req->handle].len;
    reply->is_dir = VFS_NODE_REG;
    reply->ino = 0x4000ULL + req->handle;
}

static void handle_readdir(msg_regs_t *m, tid_t from) {
    static const char *const root_names[] = {
        "cpuinfo", "filesystems", "meminfo", "mounts", "stat", "uptime", "version", "self"
    };
    static const char *const process_names[] = { "cmdline", "comm", "stat", "status" };
    const vfs_readdir_req_t *req = (const vfs_readdir_req_t *)m;
    vfs_readdir_reply_t *reply = (vfs_readdir_reply_t *)m;
    if (req->dir_ino == PROCFS_ROOT_INO) {
        uint64_t root_count = sizeof(root_names) / sizeof(root_names[0]);
        if (req->index < root_count) {
            reply->status = 0;
            reply->is_dir = req->index == root_count - 1 ? VFS_NODE_DIR : VFS_NODE_REG;
            copy_reply_name(reply, root_names[req->index]);
            return;
        }
        uint64_t want = req->index - root_count;
        uint64_t seen = 0;
        for (uint32_t tid = 1; tid < PROCFS_MAX_SCAN_TID; tid++) {
            msg_regs_t q;
            if (thread_info(tid, &q) != 0) {
                continue;
            }
            if (seen == want) {
                reply->status = 0;
                reply->is_dir = VFS_NODE_DIR;
                int pos = append_uint(reply->name, 0, VFS_NAME_MAX, tid);
                reply->name[pos] = '\0';
                return;
            }
            seen++;
        }
        reply->status = VFS_ERR_NOT_FOUND;
        return;
    }
    uint32_t tid = req->dir_ino == PROCFS_SELF_INO ? from : (uint32_t)req->dir_ino;
    if (tid == 0 || tid >= PROCFS_MAX_SCAN_TID || req->index >= sizeof(process_names) / sizeof(process_names[0])) {
        reply->status = VFS_ERR_NOT_FOUND;
        return;
    }
    msg_regs_t q;
    if (thread_info(tid, &q) != 0) {
        reply->status = VFS_ERR_NOT_FOUND;
        return;
    }
    reply->status = 0;
    reply->is_dir = VFS_NODE_REG;
    copy_reply_name(reply, process_names[req->index]);
}

void _start(void) {
    msg_regs_t m;
    tid_t from;
    for (;;) {
        ipc_recv(IPC_TID_ANY, &m, &from);
        switch (m.word[0]) {
        case VFS_OP_CAPS:
            handle_caps(&m);
            break;
        case VFS_OP_OPEN:
            handle_open(&m, from);
            break;
        case VFS_OP_READ:
            handle_read(&m);
            break;
        case VFS_OP_CLOSE:
            handle_close(&m);
            break;
        case VFS_OP_STAT:
            handle_stat(&m, from);
            break;
        case VFS_OP_FSTAT:
            handle_fstat(&m);
            break;
        case VFS_OP_READDIR:
            handle_readdir(&m, from);
            break;
        case VFS_OP_RENAME:
        case VFS_OP_UNLINK:
        case VFS_OP_SYMLINK:
        case VFS_OP_QUIESCE:
        case VFS_OP_MKDIR:
        case VFS_OP_RMDIR:
        case VFS_OP_LINK:
        case VFS_OP_MKNOD:
        case VFS_OP_XATTR:
        case VFS_OP_UTIMENS:
            m.word[0] = (uint64_t)(int64_t)VFS_ERR_NOT_SUPPORTED;
            break;
        default:
            m.word[0] = (uint64_t)(int64_t)VFS_ERR_NOT_SUPPORTED;
            break;
        }
        ipc_send(from, &m);
    }
}
