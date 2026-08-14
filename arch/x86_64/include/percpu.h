#ifndef ARCH_X86_64_PERCPU_H
#define ARCH_X86_64_PERCPU_H
#define MAX_CPUS 16
#define PERCPU_OFF_KSTACK_TOP    8
#define PERCPU_OFF_TRAP_SCRATCH  16
#ifndef __ASSEMBLER__
#include "robu/types.h"
#include "context.h"
struct tcb;
typedef struct percpu {
    struct percpu *self;
    uint64_t kstack_top;
    arch_uctx_t trap_scratch;
    struct tcb *current_thread;
    paddr_t loaded_cr3;
    uint32_t cpu_id;
    uint32_t apic_id;
} percpu_t;
static inline percpu_t *this_cpu(void) {
    percpu_t *p;
    asm volatile("movq %%gs:0, %0" : "=r"(p));
    return p;
}
void percpu_init_this_cpu(uint32_t cpu_id, uint32_t apic_id, void *kstack_top);
struct tcb *percpu_current_thread(uint32_t cpu_id);
#endif
#endif
