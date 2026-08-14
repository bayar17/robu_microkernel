#include "robu/types.h"
#include "robu/vm.h"
#include "robu/arch.h"
#include "robu/kprintf.h"
#include "robu/sched.h"
#include "context.h"
#include "lapic.h"
#include "percpu.h"
#include "portio.h"
#include "smp.h"
#include "vm_arch.h"
extern paddr_t boot_pml4;
extern percpu_t percpu_table[MAX_CPUS];
#define LAPIC_REG_ID       0x020
#define LAPIC_REG_EOI      0x0B0
#define LAPIC_REG_SVR      0x0F0
#define LAPIC_REG_ICR_LOW  0x300
#define LAPIC_REG_ICR_HIGH 0x310
#define ICR_MODE_INIT    (5u << 8)
#define ICR_MODE_STARTUP (6u << 8)
#define ICR_LEVEL_ASSERT (1u << 14)
static volatile uint32_t *lapic_reg(uint32_t offset) {
    return (volatile uint32_t *)(LAPIC_PADDR + offset);
}
static void long_delay(uint32_t spins) {
    for (uint32_t i = 0; i < spins; i++) {
        io_wait();
    }
}
void lapic_init(void) {
    arch_vm_map_page((paddr_t)&boot_pml4, LAPIC_PADDR, LAPIC_PADDR,
                     VM_PROT_READ | VM_PROT_WRITE);
    *lapic_reg(LAPIC_REG_SVR) = 0x1FF;
}
uint32_t lapic_id(void) {
    return *lapic_reg(LAPIC_REG_ID) >> 24;
}
void lapic_eoi(void) {
    *lapic_reg(LAPIC_REG_EOI) = 0;
}
void lapic_send_ipi(uint32_t target_apic_id, uint8_t vector) {
    *lapic_reg(LAPIC_REG_ICR_HIGH) = target_apic_id << 24;
    *lapic_reg(LAPIC_REG_ICR_LOW) = vector;
}
void lapic_send_init_ipi(uint32_t target_apic_id) {
    *lapic_reg(LAPIC_REG_ICR_HIGH) = target_apic_id << 24;
    *lapic_reg(LAPIC_REG_ICR_LOW) = ICR_MODE_INIT | ICR_LEVEL_ASSERT;
    long_delay(200000);
}
void lapic_send_startup_ipi(uint32_t target_apic_id, uint8_t vector_page) {
    *lapic_reg(LAPIC_REG_ICR_HIGH) = target_apic_id << 24;
    *lapic_reg(LAPIC_REG_ICR_LOW) = ICR_MODE_STARTUP | vector_page;
    long_delay(4000);
}
#define PIT_BASE_HZ 1193182u
#define PIT_CH2_DATA_PORT 0x42
#define PIT_CMD_PORT       0x43
#define PIT_GATE_SPEAKER_PORT 0x61
#define PIT_GATE_SPEAKER_ON_MASK  0x01
#define PIT_SPEAKER_DATA_MASK     0x02
#define PIT_CH2_OUT_MASK          0x20
#define CALIBRATION_WINDOW_MS 50
static uint64_t lapic_ticks_per_sched_tick;
void arch_timer_calibrate(void) {
    uint32_t divisor = (PIT_BASE_HZ / 1000u) * CALIBRATION_WINDOW_MS;
    uint8_t port61 = inb(PIT_GATE_SPEAKER_PORT);
    outb(PIT_GATE_SPEAKER_PORT,
         port61 & (uint8_t)~(PIT_GATE_SPEAKER_ON_MASK | PIT_SPEAKER_DATA_MASK));
    outb(PIT_CMD_PORT, 0xB0);
    outb(PIT_CH2_DATA_PORT, divisor & 0xFF);
    outb(PIT_CH2_DATA_PORT, (divisor >> 8) & 0xFF);
    *lapic_reg(LAPIC_REG_TIMER_DCR) = LAPIC_TIMER_DIV_16;
    *lapic_reg(LAPIC_REG_LVT_TIMER) = LAPIC_LVT_MASKED;
    *lapic_reg(LAPIC_REG_TIMER_INIT_COUNT) = 0xFFFFFFFF;
    outb(PIT_GATE_SPEAKER_PORT,
         (port61 & (uint8_t)~PIT_SPEAKER_DATA_MASK) | PIT_GATE_SPEAKER_ON_MASK);
    while (!(inb(PIT_GATE_SPEAKER_PORT) & PIT_CH2_OUT_MASK)) {
    }
    uint32_t elapsed = 0xFFFFFFFF - *lapic_reg(LAPIC_REG_TIMER_CUR_COUNT);
    lapic_ticks_per_sched_tick =
        (uint64_t)elapsed * (1000u / SCHED_HZ) / CALIBRATION_WINDOW_MS;
    kprintf("[lapic] timer calibrated: %lu LAPIC ticks per %u ms scheduler tick "
            "(%u ms window, elapsed=%u raw LAPIC ticks)\n",
            lapic_ticks_per_sched_tick, 1000u / SCHED_HZ, CALIBRATION_WINDOW_MS, elapsed);
}
void arch_timer_percpu_init(void) {
    *lapic_reg(LAPIC_REG_TIMER_DCR) = LAPIC_TIMER_DIV_16;
    *lapic_reg(LAPIC_REG_LVT_TIMER) = LAPIC_LVT_TIMER_ONESHOT | 32u;
}
void arch_timer_arm(uint64_t sched_ticks) {
    if (sched_ticks < 1) {
        sched_ticks = 1;
    }
    *lapic_reg(LAPIC_REG_TIMER_INIT_COUNT) =
        (uint32_t)(sched_ticks * lapic_ticks_per_sched_tick);
}
void arch_timer_kick_cpu(uint32_t cpu_id) {
    lapic_send_ipi(percpu_table[cpu_id].apic_id, TRAP_VEC_IPI_KICK);
}
void arch_panic_freeze_other_cores(void) {
    for (uint32_t c = 0; c < MAX_CPUS; c++) {
        if (c == this_cpu()->cpu_id) {
            continue;
        }
        if (!cpu_online[c]) {
            continue;
        }
        lapic_send_ipi(percpu_table[c].apic_id, TRAP_VEC_IPI_PANIC);
    }
}
static volatile vaddr_t  shootdown_va[MAX_CPUS];
static volatile paddr_t  shootdown_as[MAX_CPUS];
static volatile uint32_t shootdown_pending[MAX_CPUS];
tlb_shootdown_stats_t tlb_shootdown_stats;
#define SHOOTDOWN_MAX_SPINS 10000000u
void arch_tlb_shootdown_handle_local(void) {
    uint32_t me = this_cpu()->cpu_id;
    if (__atomic_load_n(&shootdown_pending[me], __ATOMIC_ACQUIRE)) {
        if (this_cpu()->loaded_cr3 == shootdown_as[me]) {
            arch_invlpg(shootdown_va[me]);
        }
        __atomic_store_n(&shootdown_pending[me], 0, __ATOMIC_RELEASE);
    }
}
void arch_tlb_shootdown(paddr_t aspace, vaddr_t va) {
    for (uint32_t c = 0; c < MAX_CPUS; c++) {
        if (c == this_cpu()->cpu_id) {
            continue;
        }
        if (!cpu_online[c]) {
            continue;
        }
        if (percpu_table[c].loaded_cr3 != aspace) {
            continue;
        }
        shootdown_va[c] = va;
        shootdown_as[c] = aspace;
        __atomic_store_n(&shootdown_pending[c], 1, __ATOMIC_RELEASE);
        lapic_send_ipi(percpu_table[c].apic_id, TRAP_VEC_IPI_SHOOTDOWN);
        tlb_shootdown_stats.sent++;
        uint32_t spins = 0;
        while (__atomic_load_n(&shootdown_pending[c], __ATOMIC_ACQUIRE) &&
               spins < SHOOTDOWN_MAX_SPINS) {
            asm volatile("pause");
            spins++;
        }
        if (spins >= SHOOTDOWN_MAX_SPINS) {
            tlb_shootdown_stats.timeouts++;
            kprintf("[lapic] WARNING: TLB shootdown to cpu %u timed out "
                    "(va=0x%lx, aspace=0x%lx) -- ack never arrived\n",
                    c, va, aspace);
        }
    }
}
