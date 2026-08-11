#include <stddef.h>
#include "robu/types.h"
#include "robu/arch.h"
#include "robu/kprintf.h"
#include "robu/tcb.h"
#include "robu/sched.h"
#include "robu/ipc.h"
#include "robu/vm.h"
#include "context.h"
#include "gdt.h"
#include "lapic.h"
void arch_uctx_init(arch_uctx_t *u, void (*entry)(void), void *stack_top) {
    memset(u, 0, sizeof(*u));
    u->rip = (uint64_t)entry;
    u->cs = 0x08;
    u->ss = 0x10;
    u->rsp = ((uint64_t)stack_top & ~0xFULL) - 8;
    u->rflags = 0x202;
}
void arch_uctx_init_user(arch_uctx_t *u, vaddr_t entry, vaddr_t user_stack_top) {
    memset(u, 0, sizeof(*u));
    u->rip = entry;
    u->cs = GDT_SEL_UCODE_RPL3;
    u->ss = GDT_SEL_UDATA_RPL3;

    u->rsp = user_stack_top & ~0xFULL;
    u->rflags = 0x202;
}
void arch_uctx_init_user_argv(arch_uctx_t *u, vaddr_t entry, vaddr_t user_stack_top,
                              uint64_t argc, uint64_t argv, uint64_t envp, uint64_t heap_base,
                              uint64_t spawn_info) {
    arch_uctx_init_user(u, entry, user_stack_top);
    u->rdi = argc;
    u->rsi = argv;
    u->rdx = envp;
    u->rcx = heap_base;
    u->r8 = spawn_info;
}
static uint64_t read_cr2(void) {
    uint64_t v;
    asm volatile("mov %%cr2, %0" : "=r"(v));
    return v;
}
static void panic_dump(arch_uctx_t *f) {
    kprintf("\n[panic] vec=%lu err=0x%lx tid=%u '%s'\n",
            f->vector, f->error, current_thread->tid, current_thread->name);
    kprintf("[panic] rip=0x%lx cs=0x%lx rflags=0x%lx rsp=0x%lx ss=0x%lx\n",
            f->rip, f->cs, f->rflags, f->rsp, f->ss);
    kprintf("[panic] rax=0x%lx rbx=0x%lx rcx=0x%lx rdx=0x%lx\n",
            f->rax, f->rbx, f->rcx, f->rdx);
    kprintf("[panic] rsi=0x%lx rdi=0x%lx rbp=0x%lx cr2=0x%lx\n",
            f->rsi, f->rdi, f->rbp, read_cr2());
    kprintf("[panic] r8=0x%lx r9=0x%lx r10=0x%lx r11=0x%lx\n",
            f->r8, f->r9, f->r10, f->r11);
    kprintf("[panic] r12=0x%lx r13=0x%lx r14=0x%lx r15=0x%lx\n",
            f->r12, f->r13, f->r14, f->r15);
    uint64_t top = (uint64_t)current_thread->kstack_top;
    uint64_t bottom = top - current_thread->kstack_size;
    kputs("[panic] best-effort backtrace (RBP chain):");
    uint64_t rbp = f->rbp;
    for (int i = 0; i < 16 && rbp; i++) {
        if (rbp < bottom || rbp > top - 16 || (rbp & 0x7) != 0) {
            kprintf("[panic]   (chain stops: rbp=0x%lx outside [0x%lx,0x%lx))\n",
                    rbp, bottom, top);
            break;
        }
        uint64_t *stack_slot = (uint64_t *)rbp;
        uint64_t saved_rbp = stack_slot[0];
        uint64_t ret_addr = stack_slot[1];
        kprintf("[panic]   #%d rip=0x%lx\n", i, ret_addr);
        rbp = saved_rbp;
    }
}
_Static_assert(sizeof(arch_uctx_t) == 176, "trap.S's frame copy size is hardcoded to match");
void trap_dispatch(arch_uctx_t *frame) {
    memcpy(&current_thread->uctx, frame, sizeof(*frame));
    if (frame->vector == TRAP_VEC_IPI_PANIC) {
        lapic_eoi();
        for (;;) {
            asm volatile("cli; hlt");
        }
    }
    if (frame->vector == TRAP_VEC_IPI_SHOOTDOWN) {
        arch_tlb_shootdown_handle_local();
        lapic_eoi();

        arch_enter_thread_raw(&current_thread->uctx);
    }
    sched_lock_acquire();
    if (arch_irq_dispatch((uint32_t)frame->vector)) {
        sched_resume();
        return;
    }
    switch (frame->vector) {
    case TRAP_VEC_TIMER:
        lapic_eoi();
        sched_tick();
        break;
    case TRAP_VEC_IPI_KICK:
        lapic_eoi();
        sched_request_resched();
        break;
    case TRAP_VEC_IPC:
        sys_ipc();
        break;
    case TRAP_VEC_PAGEFAULT:
        vm_handle_page_fault(read_cr2(), (uint32_t)frame->error);
        break;
    default:
        if (current_thread->address_space != 0) {
            if (!quiet_mode) {
                kprintf("\n[trap] vec=%lu err=0x%lx rip=0x%lx tid=%u '%s'\n",
                        frame->vector, frame->error, frame->rip,
                        current_thread->tid, current_thread->name);
                kputs("[trap] terminating the faulting service, kernel continues");
            }
            sched_terminate_current();
            break;
        }
        kprintf("\n[trap] vec=%lu err=0x%lx rip=0x%lx tid=%u '%s'\n",
                frame->vector, frame->error, frame->rip,
                current_thread->tid, current_thread->name);
        panic_dump(frame);
        arch_panic_freeze_other_cores();
        for (;;) {
            asm volatile("cli; hlt");
        }
    }
    sched_resume();
}
