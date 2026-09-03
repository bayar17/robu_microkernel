#include "ext2.h"
#include "robu/blockinfo.h"

#include "disk.c"
#include "path.c"
#include "journal.c"
#include "xattr.c"
#include "vfs.c"

static void publish_root_block_info(void) {
#ifdef EXT2FS_AS_ROOT
    msg_regs_t m = (msg_regs_t){0};
    m.word[0] = SYS_INFO_CAT_BLOCK_INFO;
    m.word[1] = BLOCK_INFO_OP_PUBLISH;
    m.word[2] = BLOCK_DEVICE_ROOT;
    m.word[3] = ((uint64_t)blkdev_transport() << 32) | BLOCK_FS_EXT2;
    m.word[4] = blkdev_capacity_sectors();
    m.word[5] = 512;
    robu_ipc_raw(0, 0, IPC_FLAG_SYS_INFO, &m, NULL);
#endif
}

static void handle_device_read(msg_regs_t *m, tid_t from) {
    const vfs_device_read_req_t *req = (const vfs_device_read_req_t *)m;
    vfs_device_read_reply_t *reply = (vfs_device_read_reply_t *)m;
#ifndef EXT2FS_AS_ROOT
    (void)from;
    (void)req;
    reply->status = VFS_ERR_NOT_SUPPORTED;
#else
    if (from != (tid_t)kinfo_user()->devfs_tid ||
        (req->device != VFS_DEVICE_ROOT_DISK && req->device != VFS_DEVICE_ROOT_PARTITION)) {
        reply->status = VFS_ERR_NOT_SUPPORTED;
        return;
    }
    uint64_t start = req->device == VFS_DEVICE_ROOT_PARTITION ? 2048ULL * 512 : 0;
    uint64_t capacity = blkdev_capacity_sectors() * 512;
    if (start >= capacity || req->offset >= capacity - start) {
        reply->status = 0;
        return;
    }
    uint64_t len = req->len > VFS_READ_MAX ? VFS_READ_MAX : req->len;
    if (len > capacity - start - req->offset) {
        len = capacity - start - req->offset;
    }
    uint64_t copied = 0;
    while (copied < len) {
        uint64_t byte_offset = req->offset + copied;
        uint64_t sector = byte_offset / 512;
        uint32_t sector_offset = (uint32_t)(byte_offset % 512);
        uint8_t sector_buf[512];
        int rc = req->device == VFS_DEVICE_ROOT_DISK
            ? blkdev_raw_read(sector, 1, sector_buf)
            : blkdev_read(sector, 1, sector_buf);
        if (rc != 0) {
            reply->status = copied ? (int64_t)copied : VFS_ERR_NOT_SUPPORTED;
            return;
        }
        uint64_t chunk = 512 - sector_offset;
        if (chunk > len - copied) {
            chunk = len - copied;
        }
        for (uint64_t i = 0; i < chunk; i++) {
            reply->data[copied + i] = sector_buf[sector_offset + i];
        }
        copied += chunk;
    }
    reply->status = (int64_t)copied;
#endif
}

void _start(void) {
    int mount_rc = ext2_mount();
    if (mount_rc != 0) {
        ipc_exit((uint8_t)(-mount_rc));
    }
    publish_root_block_info();
#ifdef EXT2FS_AS_ROOT
    seed_fixed_dirs();
#endif

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
        case VFS_OP_MKDIR:
            handle_mkdir(&m);
            break;
        case VFS_OP_SYMLINK:
            handle_symlink(&m);
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
        case VFS_OP_QUIESCE:
            handle_quiesce(&m);
            break;
        case VFS_OP_RMDIR:
            handle_rmdir(&m);
            break;
        case VFS_OP_READ_BULK:
            handle_read_bulk(&m);
            break;
        case VFS_OP_CAPS:
            handle_caps(&m);
            break;
        case VFS_OP_XATTR:
            handle_xattr(&m);
            break;
        case VFS_OP_UTIMENS:
            handle_utimens(&m);
            break;
        case VFS_OP_DEVICE_READ:
            handle_device_read(&m, from);
            break;
        default:
            m.word[0] = (uint64_t)(int64_t)VFS_ERR_NOT_SUPPORTED;
            break;
        }
        ipc_send(from, &m);
    }
}
