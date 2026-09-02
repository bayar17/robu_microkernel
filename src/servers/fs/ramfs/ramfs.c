#include "ramfs.h"
#include "robu/kinfo.h"

#include "memory.c"
#include "vfs.c"

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
        case VFS_OP_CAPS:    handle_caps(&m); break;
        case VFS_OP_XATTR:   handle_xattr(&m); break;
        case VFS_OP_UTIMENS: handle_utimens(&m); break;
        default: ((vfs_open_reply_t *)&m)->status = VFS_ERR_NOT_FOUND; break;
        }
        ipc_send(from, &m);
    }
}
