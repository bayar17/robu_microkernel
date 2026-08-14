#include "robu/types.h"
#include "robu/arch.h"
#include "robu/kprintf.h"
#include "robu/sched.h"
#include "smp.h"
#include "lapic.h"
#include "percpu.h"
#include "uacpi_glue.h"
extern const uint8_t ap_trampoline_start[];
extern const uint8_t ap_trampoline_end[];
static uint8_t ap_kstacks[MAX_CPUS][STACK_SIZE] __attribute__((aligned(16)));
static volatile uint32_t ap_alive_flag;
volatile uint32_t scheduler_ready;
volatile uint32_t cpu_online[MAX_CPUS];
volatile uint64_t ap_next_kstack_top;
volatile uint32_t ap_next_cpu_id;
void ap_entry(void) {
    uint32_t cpu_id = ap_next_cpu_id;
    percpu_init_this_cpu(cpu_id, lapic_id(), (void *)ap_next_kstack_top);
    arch_gdt_init_ap();
    arch_intr_init_ap();
    lapic_init();
    arch_timer_percpu_init();
    ap_alive_flag = 0xCAFEBABE;
    kprintf("[smp] AP cpu_id=%u apic_id=%u has its own GS-base, TSS, IDT\n",
            this_cpu()->cpu_id, this_cpu()->apic_id);
    while (!scheduler_ready) {
        asm volatile("pause");
    }
    sched_init_ap();
    cpu_online[cpu_id] = 1;
    kprintf("[smp] AP cpu_id=%u joining the scheduler\n", this_cpu()->cpu_id);
    sched_join_ap();
}
void smp_start_ap(void) {
    cpu_online[0] = 1;
    uint64_t len = (uint64_t)(ap_trampoline_end - ap_trampoline_start);
    memcpy((void *)AP_TRAMPOLINE_PADDR, ap_trampoline_start, len);

    uint32_t apic_ids[MAX_CPUS];
    int found = acpi_enumerate_cpus(apic_ids, MAX_CPUS);
    if (found <= 0) {
        kprintf("[smp] no MADT CPU topology found, running single-core\n");
        return;
    }

    uint32_t bsp_apic_id = lapic_id();
    uint32_t next_cpu_id = 1;
    for (int i = 0; i < found && next_cpu_id < MAX_CPUS; i++) {
        uint32_t target = apic_ids[i];
        if (target == bsp_apic_id) {
            continue;
        }
        ap_next_kstack_top = (uint64_t)(ap_kstacks[next_cpu_id] + STACK_SIZE);
        ap_next_cpu_id = next_cpu_id;
        ap_alive_flag = 0;
        lapic_send_init_ipi(target);
        lapic_send_startup_ipi(target, AP_TRAMPOLINE_VECTOR);
        lapic_send_startup_ipi(target, AP_TRAMPOLINE_VECTOR);
        int responded = 0;
        for (volatile uint32_t j = 0; j < 50000000; j++) {
            if (ap_alive_flag == 0xCAFEBABE) {
                responded = 1;
                break;
            }
        }
        if (responded) {
            kprintf("[smp] AP apic_id=%u online (cpu_id=%u)\n", target, next_cpu_id);
            next_cpu_id++;
        } else {
            kprintf("[smp] AP apic_id=%u did not respond within the timeout\n", target);
        }
    }
    kprintf("[smp] %u CPU(s) online\n", next_cpu_id);
}
