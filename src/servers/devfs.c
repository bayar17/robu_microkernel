#include "robu/types.h"
#include "robu/uipc.h"
#include "robu/vfs.h"
typedef enum {
    DEV_CONSOLE = 0,
    DEV_NULL = 1,
    DEV_ZERO = 2,
    DEV_RANDOM = 3,
    DEV_RSTTY1 = 4,
    DEV_RSTTY2 = 5,
    DEV_RSTTY3 = 6,
    DEV_RSTTY4 = 7,
    DEV_RSTTY5 = 8,
    DEV_RSTTY6 = 9,
    DEV_MOUSE = 10,
} dev_id_t;
#define VT_COUNT 6
#define SYS_INFO_CAT_ACTIVE_VT 16
#define SYS_INFO_CAT_CONSOLE_MODE 10
#define SYS_INFO_CAT_MOUSE_READ 23
#define VFS_ERR_INTERRUPTED (-9)
#define DEVFS_BLOCK_HANDLE_BASE 0x100
#define DEVFS_BLOCK_HANDLES 8
#define DEVFS_ROOT_PARTITION_START_LBA 2048
typedef struct {
    uint32_t info_device;
    uint32_t provider;
    uint64_t size;
} devfs_block_node_t;
typedef struct {
    int in_use;
    devfs_block_node_t node;
    uint64_t offset;
} devfs_block_handle_t;
static devfs_block_handle_t block_handles[DEVFS_BLOCK_HANDLES];
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
static int resolve_char_name(const char *name, dev_id_t *out) {
    if (name_eq(name, "console")) { *out = DEV_CONSOLE; return 0; }
    if (name_eq(name, "null"))    { *out = DEV_NULL;    return 0; }
    if (name_eq(name, "zero"))    { *out = DEV_ZERO;    return 0; }
    if (name_eq(name, "random"))  { *out = DEV_RANDOM;  return 0; }
    if (name_eq(name, "rstty1"))  { *out = DEV_RSTTY1;  return 0; }
    if (name_eq(name, "rstty2"))  { *out = DEV_RSTTY2;  return 0; }
    if (name_eq(name, "rstty3"))  { *out = DEV_RSTTY3;  return 0; }
    if (name_eq(name, "rstty4"))  { *out = DEV_RSTTY4;  return 0; }
    if (name_eq(name, "rstty5"))  { *out = DEV_RSTTY5;  return 0; }
    if (name_eq(name, "rstty6"))  { *out = DEV_RSTTY6;  return 0; }
    if (name_eq(name, "mouse"))   { *out = DEV_MOUSE;   return 0; }
    return -1;
}
static int valid_char_handle(uint64_t h) {
    return h <= DEV_MOUSE;
}
static int block_handle_index(uint64_t h) {
    if (h < DEVFS_BLOCK_HANDLE_BASE || h >= DEVFS_BLOCK_HANDLE_BASE + DEVFS_BLOCK_HANDLES) {
        return -1;
    }
    int index = (int)(h - DEVFS_BLOCK_HANDLE_BASE);
    return block_handles[index].in_use ? index : -1;
}
static int valid_handle(uint64_t h) {
    return valid_char_handle(h) || block_handle_index(h) >= 0;
}
static int block_info_at(uint32_t index, block_device_info_t *out) {
    const volatile kinfo_page_t *k = kinfo_user();
    if (index >= BLOCK_DEVICE_MAX) {
        return -1;
    }
    uint32_t seq0;
    uint32_t seq1;
    do {
        seq0 = k->block_info_seq;
        asm volatile("" ::: "memory");
        *out = k->block_devices[index];
        asm volatile("" ::: "memory");
        seq1 = k->block_info_seq;
    } while (seq0 != seq1 || (seq0 & 1u));
    return out->in_use ? 0 : -1;
}
static void copy_name(char *out, const char *in) {
    int i = 0;
    while (i < VFS_NAME_MAX - 1 && in[i]) {
        out[i] = in[i];
        i++;
    }
    out[i] = '\0';
}
static int block_base_name(const block_device_info_t *info, char *out) {
    if (info->device == BLOCK_DEVICE_ROOT) {
        if (info->transport == BLOCK_TRANSPORT_XHCI) {
            copy_name(out, "sda");
            return 0;
        }
        if (info->transport == BLOCK_TRANSPORT_IDE) {
            copy_name(out, "hda");
            return 0;
        }
        if (info->transport == BLOCK_TRANSPORT_VIRTIO) {
            copy_name(out, "vda");
            return 0;
        }
    }
    if (info->device == BLOCK_DEVICE_DATA) {
        if (info->transport == BLOCK_TRANSPORT_VIRTIO) {
            copy_name(out, "vda");
            return 0;
        }
        if (info->transport == BLOCK_TRANSPORT_IDE) {
            copy_name(out, "hdb");
            return 0;
        }
        if (info->transport == BLOCK_TRANSPORT_XHCI) {
            copy_name(out, "sdb");
            return 0;
        }
    }
    return -1;
}
static int block_node_at(uint64_t want, devfs_block_node_t *out, char *name) {
    uint64_t index = 0;
    for (uint32_t slot = 0; slot < BLOCK_DEVICE_MAX; slot++) {
        block_device_info_t info;
        char base[VFS_NAME_MAX];
        if (block_info_at(slot, &info) != 0 || block_base_name(&info, base) != 0) {
            continue;
        }
        if (index == want) {
            out->info_device = info.device;
            out->provider = info.device == BLOCK_DEVICE_ROOT ? VFS_DEVICE_ROOT_DISK :
                            VFS_DEVICE_DATA_DISK;
            out->size = info.sectors * (uint64_t)info.sector_size;
            copy_name(name, base);
            return 0;
        }
        index++;
        if (info.device == BLOCK_DEVICE_ROOT && info.sectors > DEVFS_ROOT_PARTITION_START_LBA) {
            if (index == want) {
                out->info_device = info.device;
                out->provider = VFS_DEVICE_ROOT_PARTITION;
                out->size = (info.sectors - DEVFS_ROOT_PARTITION_START_LBA) *
                            (uint64_t)info.sector_size;
                copy_name(name, base);
                int len = 0;
                while (name[len]) {
                    len++;
                }
                if (len < VFS_NAME_MAX - 1) {
                    name[len] = '1';
                    name[len + 1] = '\0';
                }
                return 0;
            }
            index++;
        }
    }
    return -1;
}
static int resolve_block_name(const char *name, devfs_block_node_t *out) {
    for (uint64_t index = 0;; index++) {
        devfs_block_node_t node;
        char node_name[VFS_NAME_MAX];
        if (block_node_at(index, &node, node_name) != 0) {
            return -1;
        }
        if (name_eq(name, node_name)) {
            *out = node;
            return 0;
        }
    }
}
static int dev_to_vt(dev_id_t id) {
    if (id == DEV_CONSOLE) {
        return 0;
    }
    if (id >= DEV_RSTTY1 && id <= DEV_RSTTY6) {
        return (int)(id - DEV_RSTTY1);
    }
    return -1;
}
static int devfs_active_vt(void) {
    msg_regs_t m = (msg_regs_t){0};
    m.word[0] = SYS_INFO_CAT_ACTIVE_VT;
    robu_ipc_raw(0, 0, IPC_FLAG_SYS_INFO, &m, NULL);
    return (int)m.word[0];
}
static int devfs_kernel_console_write(int vt, const uint8_t *buf, uint64_t len) {
    msg_regs_t m;
    uint64_t clamped = len > 40 ? 40 : len;
    m.word[0] = clamped | ((uint64_t)vt << 8);
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
static int devfs_kernel_console_read(int vt, uint8_t *buf, uint64_t max) {
    msg_regs_t m = (msg_regs_t){0};
    m.word[0] = (uint64_t)vt;
    int64_t rc = robu_ipc_raw(0, 0, IPC_FLAG_CONSOLE_READ, &m, NULL);
    if (rc == IPC_ERR_CANCELED) {
        return VFS_ERR_INTERRUPTED;
    }
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
static uint8_t console_local_buf[VT_COUNT][CONSOLE_LOCAL_BUF_SIZE];
static uint32_t console_local_head[VT_COUNT], console_local_tail[VT_COUNT];
static int console_eof_pending[VT_COUNT];
static int devfs_console_raw_mode(int vt) {
    msg_regs_t m = (msg_regs_t){0};
    m.word[0] = SYS_INFO_CAT_CONSOLE_MODE;
    m.word[1] = 2;
    m.word[2] = (uint64_t)vt;
    robu_ipc_raw(0, 0, IPC_FLAG_SYS_INFO, &m, NULL);
    return (int)m.word[0];
}
static void console_local_discard(int vt) {
    console_local_head[vt] = 0;
    console_local_tail[vt] = 0;
}
static void console_local_push(int vt, uint8_t byte) {
    uint32_t next = (console_local_head[vt] + 1) % CONSOLE_LOCAL_BUF_SIZE;
    if (next == console_local_tail[vt]) {
        return;
    }
    console_local_buf[vt][console_local_head[vt]] = byte;
    console_local_head[vt] = next;
}
static void console_drain(int vt) {
    if (console_local_head[vt] != console_local_tail[vt]) {
        return;
    }
    uint8_t kbuf[VFS_READ_MAX];
    int got = devfs_kernel_console_read(vt, kbuf, sizeof(kbuf));
    if (got <= 0) {
        return;
    }
    int cooked = !devfs_console_raw_mode(vt);
    for (int i = 0; i < got; i++) {
        uint8_t byte = kbuf[i];
        if (byte == 0x03) {
            console_local_discard(vt);
            console_eof_pending[vt] = 0;
            continue;
        }
        if (byte == 0x04 && cooked) {
            console_eof_pending[vt] = 1;
            continue;
        }
        console_local_push(vt, byte);
    }
}
static int devfs_kernel_mouse_read(uint64_t *out, int max) {
    msg_regs_t m = (msg_regs_t){0};
    m.word[0] = SYS_INFO_CAT_MOUSE_READ;
    robu_ipc_raw(0, 0, IPC_FLAG_SYS_INFO, &m, NULL);
    int n = (int)m.word[0];
    if (n > 4) {
        n = 4;
    }
    uint64_t events[4] = { m.word[1], m.word[2], m.word[3], m.word[4] };
    if (n > max) {
        n = max;
    }
    for (int i = 0; i < n; i++) {
        out[i] = events[i];
    }
    return n;
}
#define MOUSE_LOCAL_BUF_EVENTS 16
static uint64_t mouse_local_buf[MOUSE_LOCAL_BUF_EVENTS];
static uint32_t mouse_local_head, mouse_local_tail;
static void mouse_drain(void) {
    if (mouse_local_head != mouse_local_tail) {
        return;
    }
    uint64_t events[4];
    int got = devfs_kernel_mouse_read(events, 4);
    for (int i = 0; i < got; i++) {
        uint32_t next = (mouse_local_head + 1) % MOUSE_LOCAL_BUF_EVENTS;
        if (next == mouse_local_tail) {
            break;
        }
        mouse_local_buf[mouse_local_head] = events[i];
        mouse_local_head = next;
    }
}
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
    if (resolve_char_name(name, &id) == 0) {
        reply->status = 0;
        reply->handle = (uint64_t)id;
        return;
    }
    devfs_block_node_t node;
    if (resolve_block_name(name, &node) != 0) {
        reply->status = VFS_ERR_NOT_FOUND;
        return;
    }
    for (int i = 0; i < DEVFS_BLOCK_HANDLES; i++) {
        if (!block_handles[i].in_use) {
            block_handles[i].in_use = 1;
            block_handles[i].node = node;
            block_handles[i].offset = 0;
            reply->status = 0;
            reply->handle = DEVFS_BLOCK_HANDLE_BASE + (uint64_t)i;
            return;
        }
    }
    reply->status = VFS_ERR_NO_SPACE;
}
static int console_fg_read_allowed(tid_t from, int vt) {
    if (vt != devfs_active_vt()) {
        return 0;
    }
    msg_regs_t q = (msg_regs_t){0};
    q.word[0] = SYS_INFO_CAT_TCGETPGRP;
    q.word[1] = (uint64_t)vt;
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
    int block_index = block_handle_index(handle);
    if (!valid_char_handle(handle) && block_index < 0) {
        reply->status = VFS_ERR_BAD_HANDLE;
        return;
    }
    if (block_index >= 0) {
        devfs_block_handle_t *hd = &block_handles[block_index];
        if (hd->offset >= hd->node.size) {
            reply->status = 0;
            return;
        }
        if (len > hd->node.size - hd->offset) {
            len = hd->node.size - hd->offset;
        }
        tid_t provider_tid;
        if (hd->node.provider == VFS_DEVICE_ROOT_DISK ||
            hd->node.provider == VFS_DEVICE_ROOT_PARTITION) {
            provider_tid = (tid_t)kinfo_user()->ext2fs_tid;
        } else if (hd->node.provider == VFS_DEVICE_DATA_DISK) {
            provider_tid = (tid_t)kinfo_user()->diskfs_tid;
        } else {
            provider_tid = 0;
        }
        if (provider_tid == 0) {
            reply->status = VFS_ERR_NOT_SUPPORTED;
            return;
        }
        int64_t n = vfs_device_read(provider_tid, hd->node.provider,
                                    hd->offset, reply->data, len);
        if (n > 0) {
            hd->offset += (uint64_t)n;
        }
        reply->status = n;
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
    case DEV_CONSOLE:
    case DEV_RSTTY1:
    case DEV_RSTTY2:
    case DEV_RSTTY3:
    case DEV_RSTTY4:
    case DEV_RSTTY5:
    case DEV_RSTTY6: {
        int vt = dev_to_vt((dev_id_t)handle);
        if (!console_fg_read_allowed(from, vt)) {
            reply->status = 0;
            break;
        }
        console_drain(vt);
        if (console_local_head[vt] == console_local_tail[vt]) {
            if (console_eof_pending[vt]) {
                console_eof_pending[vt] = 0;
                reply->status = 0;
            } else {
                reply->status = VFS_ERR_WOULDBLOCK;
            }
            break;
        }
        int n = 0;
        while ((uint64_t)n < len && console_local_head[vt] != console_local_tail[vt]) {
            reply->data[n++] = console_local_buf[vt][console_local_tail[vt]];
            console_local_tail[vt] = (console_local_tail[vt] + 1) % CONSOLE_LOCAL_BUF_SIZE;
        }
        reply->status = n;
        break;
    }
    case DEV_MOUSE: {
        mouse_drain();
        if (mouse_local_head == mouse_local_tail) {
            reply->status = VFS_ERR_WOULDBLOCK;
            break;
        }
        int max_events = (int)(len / 8);
        if (max_events < 1) {
            max_events = 1;
        }
        int n = 0;
        while (n < max_events && mouse_local_head != mouse_local_tail) {
            uint64_t ev = mouse_local_buf[mouse_local_tail];
            mouse_local_tail = (mouse_local_tail + 1) % MOUSE_LOCAL_BUF_EVENTS;
            for (int b = 0; b < 8; b++) {
                reply->data[n * 8 + b] = (uint8_t)(ev >> (b * 8));
            }
            n++;
        }
        reply->status = n * 8;
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
    if (block_handle_index(handle) >= 0) {
        reply->status = 1;
        return;
    }
    switch ((dev_id_t)handle) {
    case DEV_NULL:
    case DEV_ZERO:
    case DEV_RANDOM:
        reply->status = 1;
        break;
    case DEV_CONSOLE:
    case DEV_RSTTY1:
    case DEV_RSTTY2:
    case DEV_RSTTY3:
    case DEV_RSTTY4:
    case DEV_RSTTY5:
    case DEV_RSTTY6: {
        int vt = dev_to_vt((dev_id_t)handle);
        if (!console_fg_read_allowed(from, vt)) {
            reply->status = 0;
            break;
        }
        console_drain(vt);
        reply->status = (console_local_head[vt] != console_local_tail[vt] || console_eof_pending[vt]) ? 1 : 0;
        break;
    }
    case DEV_MOUSE:
        mouse_drain();
        reply->status = (mouse_local_head != mouse_local_tail) ? 1 : 0;
        break;
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
    if (block_handle_index(handle) >= 0) {
        reply->status = VFS_ERR_NOT_SUPPORTED;
        return;
    }
    switch ((dev_id_t)handle) {
    case DEV_NULL:
    case DEV_ZERO:
    case DEV_RANDOM:
        reply->status = (int64_t)len;
        break;
    case DEV_CONSOLE:
    case DEV_RSTTY1:
    case DEV_RSTTY2:
    case DEV_RSTTY3:
    case DEV_RSTTY4:
    case DEV_RSTTY5:
    case DEV_RSTTY6:
        devfs_kernel_console_write(dev_to_vt((dev_id_t)handle), data, len);
        reply->status = (int64_t)len;
        break;
    case DEV_MOUSE:
        reply->status = VFS_ERR_NOT_SUPPORTED;
        break;
    }
}
static void handle_close(msg_regs_t *m) {
    const vfs_close_req_t *req = (const vfs_close_req_t *)m;
    uint64_t handle = req->handle;
    vfs_close_reply_t *reply = (vfs_close_reply_t *)m;
    int block_index = block_handle_index(handle);
    if (block_index >= 0) {
        block_handles[block_index].in_use = 0;
        reply->status = 0;
        return;
    }
    reply->status = valid_char_handle(handle) ? 0 : VFS_ERR_BAD_HANDLE;
}
static void handle_readdir(msg_regs_t *m) {
    static const char *const names[] = { "console", "null", "zero", "random",
                                          "rstty1", "rstty2", "rstty3", "rstty4", "rstty5", "rstty6",
                                          "mouse" };
    const vfs_readdir_req_t *req = (const vfs_readdir_req_t *)m;
    uint64_t want = req->index;
    vfs_readdir_reply_t *reply = (vfs_readdir_reply_t *)m;
    if (want < sizeof(names) / sizeof(names[0])) {
        reply->status = 0;
        reply->is_dir = VFS_NODE_CHAR;
        int i = 0;
        while (names[want][i]) {
            reply->name[i] = names[want][i];
            i++;
        }
        reply->name[i] = '\0';
        return;
    }
    devfs_block_node_t node;
    char name[VFS_NAME_MAX];
    if (block_node_at(want - sizeof(names) / sizeof(names[0]), &node, name) != 0) {
        reply->status = VFS_ERR_NOT_FOUND;
        return;
    }
    reply->status = 0;
    reply->is_dir = VFS_NODE_BLOCK;
    int i = 0;
    while (name[i]) {
        reply->name[i] = name[i];
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
        reply->is_dir = VFS_NODE_DIR;
        reply->ino = 0;
        return;
    }
    dev_id_t id;
    if (resolve_char_name(name, &id) == 0) {
        reply->status = 0;
        reply->size = 0;
        reply->is_dir = VFS_NODE_CHAR;
        reply->ino = (uint64_t)id + 1;
        return;
    }
    devfs_block_node_t node;
    if (resolve_block_name(name, &node) != 0) {
        reply->status = VFS_ERR_NOT_FOUND;
        return;
    }
    reply->status = 0;
    reply->size = node.size;
    reply->is_dir = VFS_NODE_BLOCK;
    reply->ino = 0x100 + node.info_device * 2 +
                 (node.provider == VFS_DEVICE_ROOT_PARTITION ? 1 : 0);
}
static void handle_fstat(msg_regs_t *m) {
    const vfs_fstat_req_t *req = (const vfs_fstat_req_t *)m;
    uint64_t handle = req->handle;
    vfs_stat_reply_t *reply = (vfs_stat_reply_t *)m;
    int block_index = block_handle_index(handle);
    if (!valid_char_handle(handle) && block_index < 0) {
        reply->status = VFS_ERR_BAD_HANDLE;
        return;
    }
    if (block_index >= 0) {
        devfs_block_node_t *node = &block_handles[block_index].node;
        reply->status = 0;
        reply->size = node->size;
        reply->is_dir = VFS_NODE_BLOCK;
        reply->ino = 0x100 + node->info_device * 2 +
                     (node->provider == VFS_DEVICE_ROOT_PARTITION ? 1 : 0);
        return;
    }
    reply->status = 0;
    reply->size = 0;
    reply->is_dir = VFS_NODE_CHAR;
    reply->ino = handle + 1;
}
static void handle_seek(msg_regs_t *m) {
    const vfs_seek_req_t *req = (const vfs_seek_req_t *)m;
    vfs_seek_reply_t *reply = (vfs_seek_reply_t *)m;
    int block_index = block_handle_index(req->handle);
    if (block_index < 0) {
        reply->status = VFS_ERR_NOT_SUPPORTED;
        return;
    }
    devfs_block_handle_t *hd = &block_handles[block_index];
    uint64_t base;
    if (req->whence == VFS_SEEK_SET) {
        base = 0;
    } else if (req->whence == VFS_SEEK_CUR) {
        base = hd->offset;
    } else if (req->whence == VFS_SEEK_END) {
        base = hd->node.size;
    } else {
        reply->status = VFS_ERR_INVALID;
        return;
    }
    uint64_t offset;
    if (req->offset < 0) {
        uint64_t amount = (uint64_t)(-(req->offset + 1)) + 1;
        if (amount > base) {
            reply->status = VFS_ERR_INVALID;
            return;
        }
        offset = base - amount;
    } else {
        uint64_t amount = (uint64_t)req->offset;
        if (amount > hd->node.size - base) {
            reply->status = VFS_ERR_INVALID;
            return;
        }
        offset = base + amount;
    }
    hd->offset = offset;
    reply->status = 0;
    reply->offset = offset;
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
        case VFS_OP_SEEK:
            handle_seek(&m);
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
