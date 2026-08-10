#include "robu/types.h"
#include "robu/uipc.h"
#include "robu/ipc.h"
#include "robu/vm.h"
#include "robu/pmm.h"
void _start(void) {
    msg_regs_t m;
    tid_t from;
    int color_cursor = 0;
    for (;;) {
        ipc_recv(IPC_TID_ANY, &m, &from);
        vaddr_t fault_addr = m.word[0];
        tid_t faulter = (tid_t)m.word[2];
        vaddr_t page_va = fault_addr & ~(PAGE_SIZE_4K - 1);
        int color = color_cursor++ & (PMM_NUM_COLORS - 1);
        msg_regs_t resolve = (msg_regs_t){0};
        resolve.word[0] = faulter;
        resolve.word[1] = page_va;
        resolve.word[2] = (uint64_t)color;
        resolve.word[3] = VM_PROT_READ | VM_PROT_WRITE | VM_PROT_USER;
        robu_ipc_raw(0, 0, IPC_FLAG_RESOLVE_FAULT, &resolve, NULL);
        ipc_send(faulter, NULL);
    }
}
