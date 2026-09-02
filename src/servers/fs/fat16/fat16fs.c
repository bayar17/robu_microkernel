#include "fat16.h"

#include "disk.c"
#include "vfs.c"

void _start(void) {
    if (fat16_mount() != 0) {
        ipc_exit(1);
    }

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
        case VFS_OP_CAPS:
            handle_caps(&m);
            break;
        case VFS_OP_UTIMENS:
            handle_utimens(&m);
            break;
        default:
            m.word[0] = (uint64_t)(int64_t)VFS_ERR_NOT_SUPPORTED;
            break;
        }
        ipc_send(from, &m);
    }
}
