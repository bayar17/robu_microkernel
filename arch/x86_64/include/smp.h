#ifndef ARCH_X86_64_SMP_H
#define ARCH_X86_64_SMP_H
#define AP_TRAMPOLINE_PADDR 0x8000
#define AP_TRAMPOLINE_VECTOR 0x08
#ifndef __ASSEMBLER__
#include "robu/types.h"
#include "percpu.h"
void smp_start_ap(void);
extern volatile uint32_t scheduler_ready;
extern volatile uint32_t cpu_online[MAX_CPUS];
#endif
#endif
