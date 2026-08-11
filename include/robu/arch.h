#ifndef ROBU_ARCH_H
#define ROBU_ARCH_H
#include "robu/types.h"
void arch_intr_init(void);
void arch_timer_calibrate(void);
void arch_timer_percpu_init(void);
void arch_timer_arm(uint64_t sched_ticks);
void arch_timer_kick_cpu(uint32_t cpu_id);
void arch_panic_freeze_other_cores(void);
void arch_tlb_shootdown(paddr_t aspace, vaddr_t va);
void arch_tlb_shootdown_handle_local(void);
typedef void (*arch_irq_handler_t)(void *ctx);
int arch_irq_register(uint32_t irq, arch_irq_handler_t handler, void *ctx);
void arch_irq_unregister(uint32_t irq);
int arch_irq_dispatch(uint32_t vector);
void arch_reboot(void) __attribute__((noreturn));
void arch_halt(void) __attribute__((noreturn));
void arch_shutdown(void) __attribute__((noreturn));
typedef struct {
    uint64_t sent;
    uint64_t timeouts;
} tlb_shootdown_stats_t;
extern tlb_shootdown_stats_t tlb_shootdown_stats;
void arch_idle(void);
uint64_t arch_irq_save(void);
void arch_irq_restore(uint64_t flags);
void arch_detect_memory(paddr_t *out_base, uint64_t *out_len);
const char *arch_boot_cmdline(void);
int arch_boot_magic(uint32_t *out_magic);
void arch_test_exit(int code) __attribute__((noreturn));
int arch_boot_module(paddr_t *out_base, uint64_t *out_len);
void arch_gdt_init(void);
void arch_gdt_init_ap(void);
void arch_intr_init_ap(void);
void arch_vm_activate(paddr_t aspace);
typedef struct {
    uint64_t loads;
    uint64_t skips;
} vm_activate_stats_t;
extern vm_activate_stats_t vm_activate_stats;
void *memset(void *s, int c, size_t n);
void *memcpy(void *dest, const void *src, size_t n);
void *memmove(void *dest, const void *src, size_t n);
size_t strlen(const char *s);
int strncmp(const char *a, const char *b, size_t n);
int memcmp(const void *a, const void *b, size_t n);
int arch_boot_rsdp(paddr_t *out_rsdp);
#endif
