#include "robu/types.h"
#include "robu/uipc.h"
#include "robu/ipc.h"
#include "robu/vfs.h"

void _start(void) {
    vfs_quiesce_disk();
    msg_regs_t m = (msg_regs_t){0};
    m.word[0] = 4;
    robu_ipc_raw(0, 0, IPC_FLAG_SYS_INFO, &m, NULL);
    ipc_exit(1);
}
