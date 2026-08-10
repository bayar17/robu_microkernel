#include "robu/types.h"
#include "robu/uipc.h"
#include "robu/ipc.h"
#include "robu/vfs.h"
#define DISKFS_MAX_FILES     24
#define DISKFS_MAX_HANDLES   16
#define DISKFS_NAME_MAX      32
#define DISKFS_DIRECT_BLOCKS 16
#define DISKFS_BLOCK_SIZE    4096
#define DISKFS_SECTORS_PER_BLOCK (DISKFS_BLOCK_SIZE / 512)
#define DISKFS_ROOT_INO 1
#define DISKFS_SUPERBLOCK_BLOCK 0
#define DISKFS_FILETABLE_BLOCK  1
#define DISKFS_DATA_START_BLOCK 2
#define DISKFS_MAGIC 0x524F42555F444653ULL
#define DISKFS_VERSION 1
typedef struct {
    uint64_t magic;
    uint32_t version;
    uint32_t total_blocks;
    uint32_t file_table_block;
    uint32_t clean_shutdown;
} __attribute__((packed)) diskfs_superblock_t;
typedef struct {
    uint32_t in_use;
    uint32_t is_dir;
    uint32_t parent_ino;
    uint32_t size;
    char name[DISKFS_NAME_MAX];
    uint32_t blocks[DISKFS_DIRECT_BLOCKS];
} __attribute__((packed)) diskfs_file_record_t;
_Static_assert(sizeof(diskfs_file_record_t) * DISKFS_MAX_FILES <= DISKFS_BLOCK_SIZE,
               "file table must fit in one block");
typedef struct {
    int in_use;
    int file_idx;
    uint64_t offset;
} diskfs_handle_t;
static diskfs_file_record_t files[DISKFS_MAX_FILES];
static diskfs_handle_t handles[DISKFS_MAX_HANDLES];
static diskfs_superblock_t g_sb;

static void copy_bytes(void *dst_, const void *src_, uint64_t n) {
    uint8_t *dst = (uint8_t *)dst_;
    const uint8_t *src = (const uint8_t *)src_;
    for (uint64_t i = 0; i < n; i++) {
        dst[i] = src[i];
    }
}
static void zero_bytes(void *dst_, uint64_t n) {
    uint8_t *dst = (uint8_t *)dst_;
    for (uint64_t i = 0; i < n; i++) {
        dst[i] = 0;
    }
}

#define BLK_CHUNK_READ_MAX 40
#define BLK_CHUNK_WRITE_MAX 24
static int blk_load(uint32_t block_num) {
    msg_regs_t m = (msg_regs_t){0};
    m.word[0] = 0;
    m.word[1] = (uint64_t)block_num * DISKFS_SECTORS_PER_BLOCK;
    m.word[2] = DISKFS_SECTORS_PER_BLOCK;
    int64_t rc = robu_ipc_raw(0, 0, IPC_FLAG_BLK_IO, &m, NULL);
    return rc == IPC_ERR_NONE ? 0 : -1;
}
static int blk_read_chunk(uint32_t offset, uint8_t *out, uint32_t *len_out) {
    msg_regs_t m = (msg_regs_t){0};
    m.word[0] = 1;
    m.word[1] = offset;
    int64_t n = robu_ipc_raw(0, 0, IPC_FLAG_BLK_IO, &m, NULL);
    if (n < 0) {
        return -1;
    }
    uint64_t words[5] = { m.word[0], m.word[1], m.word[2], m.word[3], m.word[4] };
    copy_bytes(out, words, n);
    *len_out = (uint32_t)n;
    return 0;
}
static int blk_write_chunk(uint32_t offset, const uint8_t *data, uint32_t len) {
    msg_regs_t m = (msg_regs_t){0};
    m.word[0] = 2;
    m.word[1] = offset;
    m.word[2] = len;
    uint8_t buf[BLK_CHUNK_WRITE_MAX] = {0};
    copy_bytes(buf, data, len);
    uint64_t words[3];
    copy_bytes(words, buf, BLK_CHUNK_WRITE_MAX);
    m.word[3] = words[0];
    m.word[4] = words[1];
    m.word[5] = words[2];
    int64_t rc = robu_ipc_raw(0, 0, IPC_FLAG_BLK_IO, &m, NULL);
    return rc == IPC_ERR_NONE ? 0 : -1;
}
static int blk_commit(uint32_t block_num) {
    msg_regs_t m = (msg_regs_t){0};
    m.word[0] = 3;
    m.word[1] = (uint64_t)block_num * DISKFS_SECTORS_PER_BLOCK;
    m.word[2] = DISKFS_SECTORS_PER_BLOCK;
    int64_t rc = robu_ipc_raw(0, 0, IPC_FLAG_BLK_IO, &m, NULL);
    return rc == IPC_ERR_NONE ? 0 : -1;
}
static int diskfs_read_block(uint32_t block_num, uint8_t *buf) {
    if (blk_load(block_num) != 0) {
        return -1;
    }
    uint32_t off = 0;
    while (off < DISKFS_BLOCK_SIZE) {
        uint32_t n = 0;
        if (blk_read_chunk(off, buf + off, &n) != 0) {
            return -1;
        }
        if (n == 0) {
            break;
        }
        off += n;
    }
    return 0;
}
static int diskfs_write_block(uint32_t block_num, const uint8_t *buf) {
    uint32_t off = 0;
    while (off < DISKFS_BLOCK_SIZE) {
        uint32_t len = DISKFS_BLOCK_SIZE - off;
        if (len > BLK_CHUNK_WRITE_MAX) {
            len = BLK_CHUNK_WRITE_MAX;
        }
        if (blk_write_chunk(off, buf + off, len) != 0) {
            return -1;
        }
        off += len;
    }
    return blk_commit(block_num);
}

static void persist_file_table(void) {
    uint8_t buf[DISKFS_BLOCK_SIZE];
    zero_bytes(buf, sizeof(buf));
    copy_bytes(buf, files, sizeof(files));
    diskfs_write_block(DISKFS_FILETABLE_BLOCK, buf);
}
static void load_file_table(void) {
    uint8_t buf[DISKFS_BLOCK_SIZE];
    diskfs_read_block(DISKFS_FILETABLE_BLOCK, buf);
    copy_bytes(files, buf, sizeof(files));
}
static void persist_superblock(void) {
    uint8_t buf[DISKFS_BLOCK_SIZE];
    zero_bytes(buf, sizeof(buf));
    copy_bytes(buf, &g_sb, sizeof(g_sb));
    diskfs_write_block(DISKFS_SUPERBLOCK_BLOCK, buf);
}
static uint32_t file_block_num(int file_idx, int direct_idx) {
    return DISKFS_DATA_START_BLOCK + (uint32_t)file_idx * DISKFS_DIRECT_BLOCKS + (uint32_t)direct_idx;
}
static void diskfs_boot(void) {
    uint8_t buf[DISKFS_BLOCK_SIZE];
    diskfs_read_block(DISKFS_SUPERBLOCK_BLOCK, buf);
    copy_bytes(&g_sb, buf, sizeof(g_sb));
    if (g_sb.magic != DISKFS_MAGIC) {
        zero_bytes(files, sizeof(files));
        persist_file_table();
        g_sb.magic = DISKFS_MAGIC;
        g_sb.version = DISKFS_VERSION;
        g_sb.total_blocks = 0;
        g_sb.file_table_block = DISKFS_FILETABLE_BLOCK;
        g_sb.clean_shutdown = 0;
        persist_superblock();
    } else {
        load_file_table();
        g_sb.clean_shutdown = 0;
        persist_superblock();
    }
}
static void handle_quiesce(msg_regs_t *m) {
    vfs_quiesce_reply_t *reply = (vfs_quiesce_reply_t *)m;
    g_sb.clean_shutdown = 1;
    persist_superblock();
    reply->status = 0;
}

static int name_eq(const char *a, const char *b) {
    for (int i = 0; i < DISKFS_NAME_MAX; i++) {
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
    for (int i = 0; i < DISKFS_MAX_FILES; i++) {
        if (files[i].in_use && name_eq(files[i].name, name)) {
            return i;
        }
    }
    return -1;
}
static int alloc_file(void) {
    for (int i = 0; i < DISKFS_MAX_FILES; i++) {
        if (!files[i].in_use) {
            return i;
        }
    }
    return -1;
}
static int alloc_handle(void) {
    for (int i = 0; i < DISKFS_MAX_HANDLES; i++) {
        if (!handles[i].in_use) {
            return i;
        }
    }
    return -1;
}
static int valid_handle(uint64_t h) {
    return h < DISKFS_MAX_HANDLES && handles[h].in_use;
}

static void handle_open(msg_regs_t *m) {
    char name[VFS_PATH_MAX];
    const vfs_open_req_t *req = (const vfs_open_req_t *)m;
    uint64_t flags = req->flags;
    for (int i = 0; i < VFS_PATH_MAX; i++) {
        name[i] = req->name[i];
    }
    vfs_open_reply_t *reply = (vfs_open_reply_t *)m;
    int fidx = find_file(name);
    if (fidx >= 0 && files[fidx].is_dir) {
        reply->status = VFS_ERR_IS_DIR;
        return;
    }
    if (fidx < 0) {
        if (!(flags & VFS_O_CREAT)) {
            reply->status = VFS_ERR_NOT_FOUND;
            return;
        }
        fidx = alloc_file();
        if (fidx < 0) {
            reply->status = VFS_ERR_NO_SPACE;
            return;
        }
        files[fidx].in_use = 1;
        files[fidx].is_dir = 0;
        files[fidx].parent_ino = DISKFS_ROOT_INO;
        files[fidx].size = 0;
        set_name(files[fidx].name, name, sizeof(files[fidx].name));
        for (int j = 0; j < DISKFS_DIRECT_BLOCKS; j++) {
            files[fidx].blocks[j] = file_block_num(fidx, j);
        }
        persist_file_table();
    } else if (flags & VFS_O_TRUNC) {
        files[fidx].size = 0;
        persist_file_table();
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
    diskfs_handle_t *hd = &handles[h];
    diskfs_file_record_t *f = &files[hd->file_idx];
    uint64_t avail = f->size > hd->offset ? f->size - hd->offset : 0;
    uint64_t n = len < avail ? len : avail;
    uint64_t got = 0;
    static uint8_t blockbuf[DISKFS_BLOCK_SIZE];
    while (got < n) {
        uint64_t file_off = hd->offset + got;
        uint32_t block_idx = (uint32_t)(file_off / DISKFS_BLOCK_SIZE);
        uint32_t block_off = (uint32_t)(file_off % DISKFS_BLOCK_SIZE);
        if (block_idx >= DISKFS_DIRECT_BLOCKS) {
            break;
        }
        if (diskfs_read_block(f->blocks[block_idx], blockbuf) != 0) {
            break;
        }
        uint64_t chunk = DISKFS_BLOCK_SIZE - block_off;
        if (chunk > n - got) {
            chunk = n - got;
        }
        copy_bytes(reply->data + got, blockbuf + block_off, chunk);
        got += chunk;
    }
    hd->offset += got;
    reply->status = (int64_t)got;
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
    diskfs_handle_t *hd = &handles[h];
    diskfs_file_record_t *f = &files[hd->file_idx];
    uint64_t got = 0;
    static uint8_t blockbuf[DISKFS_BLOCK_SIZE];
    while (got < len) {
        uint64_t file_off = hd->offset + got;
        uint32_t block_idx = (uint32_t)(file_off / DISKFS_BLOCK_SIZE);
        uint32_t block_off = (uint32_t)(file_off % DISKFS_BLOCK_SIZE);
        if (block_idx >= DISKFS_DIRECT_BLOCKS) {
            break;
        }
        if (diskfs_read_block(f->blocks[block_idx], blockbuf) != 0) {
            break;
        }
        uint64_t chunk = DISKFS_BLOCK_SIZE - block_off;
        if (chunk > len - got) {
            chunk = len - got;
        }
        copy_bytes(blockbuf + block_off, data + got, chunk);
        if (diskfs_write_block(f->blocks[block_idx], blockbuf) != 0) {
            break;
        }
        got += chunk;
    }
    hd->offset += got;
    if (hd->offset > f->size) {
        f->size = (uint32_t)hd->offset;
    }
    if (got > 0) {
        persist_file_table();
    }
    reply->status = (int64_t)got;
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
        reply->ino = DISKFS_ROOT_INO;
        return;
    }
    int fidx = find_file(name);
    if (fidx < 0) {
        reply->status = VFS_ERR_NOT_FOUND;
        return;
    }
    reply->status = 0;
    reply->size = files[fidx].size;
    reply->is_dir = files[fidx].is_dir;
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
    diskfs_file_record_t *f = &files[handles[h].file_idx];
    reply->status = 0;
    reply->size = f->size;
    reply->is_dir = f->is_dir;
    reply->ino = (uint64_t)handles[h].file_idx + 2;
}
static void handle_readdir(msg_regs_t *m) {
    const vfs_readdir_req_t *req = (const vfs_readdir_req_t *)m;
    uint64_t dir_ino = req->dir_ino;
    uint64_t want = req->index;
    vfs_readdir_reply_t *reply = (vfs_readdir_reply_t *)m;
    uint64_t seen = 0;
    for (int i = 0; i < DISKFS_MAX_FILES; i++) {
        if (!files[i].in_use || files[i].parent_ino != dir_ino) {
            continue;
        }
        if (seen == want) {
            reply->status = 0;
            reply->is_dir = files[i].is_dir;
            set_name(reply->name, files[i].name, sizeof(reply->name));
            return;
        }
        seen++;
    }
    reply->status = VFS_ERR_NOT_FOUND;
}
void _start(void) {
    diskfs_boot();
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
        case VFS_OP_QUIESCE:
            handle_quiesce(&m);
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
