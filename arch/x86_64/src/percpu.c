#include <stddef.h>
#include "percpu.h"
percpu_t percpu_table[MAX_CPUS];
_Static_assert(offsetof(percpu_t, kstack_top) == PERCPU_OFF_KSTACK_TOP,
               "trap.S reads kstack_top at a hardcoded gs-relative offset");
_Static_assert(offsetof(percpu_t, trap_scratch) == PERCPU_OFF_TRAP_SCRATCH,
               "trap.S writes trap_scratch at a hardcoded gs-relative offset");
#define IA32_GS_BASE 0xC0000101u
static void wrmsr64(uint32_t msr, uint64_t value) {
    uint32_t lo = (uint32_t)value;
    uint32_t hi = (uint32_t)(value >> 32);
    asm volatile("wrmsr" : : "c"(msr), "a"(lo), "d"(hi));
}
struct tcb *percpu_current_thread(uint32_t cpu_id) {
    return percpu_table[cpu_id].current_thread;
}
void percpu_init_this_cpu(uint32_t cpu_id, uint32_t apic_id, void *kstack_top) {
    percpu_t *p = &percpu_table[cpu_id];
    p->self = p;
    p->kstack_top = (uint64_t)kstack_top;
    p->current_thread = NULL;
    p->loaded_cr3 = (paddr_t)-1;
    p->cpu_id = cpu_id;
    p->apic_id = apic_id;
    wrmsr64(IA32_GS_BASE, (uint64_t)p);
}
