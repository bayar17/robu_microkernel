#include "robu/types.h"
#include "robu/arch.h"
#include "robu/kprintf.h"
#include "robu/sched.h"
#include "robu/vm.h"
#include "robu/pmm.h"
#include "robu/uipc.h"
#include "robu/kinfo.h"
#include "robu/signal.h"
#include "robu/cmdline.h"
#include "robu/untyped.h"
#include "robu/dma.h"
#include "robu/kheap.h"
#include "robu/virtio_blk.h"
#include "lapic.h"
#include "smp.h"
#include "percpu.h"
#include "pci.h"
#include "boot.h"

#define UNTYPED_REGION_SIZE (128u * 1024)
#define DMA_REGION_SIZE (256u * 1024)
#define ACPI_HEAP_REGION_SIZE (2u * 1024 * 1024)

int quiet_mode;

static uint8_t stack_console_in[STACK_SIZE] __attribute__((aligned(16)));
static void console_in_entry(void) {
    for (;;) {
        int c;
        while ((c = arch_console_getc()) >= 0) {
            arch_console_line_feed(c);
        }
        ipc_sleep(1);
    }
}

static uint8_t stack_monitor[STACK_SIZE] __attribute__((aligned(16)));
static void monitor_entry(void) {
    msg_regs_t m;
    tid_t from;
    uint64_t total = 0;
    for (;;) {
        ipc_recv(IPC_TID_ANY, &m, &from);
        total++;
        if (total % 25 == 0) {
            SAFE_PRINT("\033[36m[monitor]\033[0m ring3 tid=%u ping #%lu addr=0x%lx kinfo tick=%lu\n",
                       from, m.word[0], m.word[1], m.word[3]);
        }
    }
}

static int blk_self_test_one(uint64_t sector, uint32_t count, uint8_t seed) {
    static uint8_t pattern[8 * 512];
    static uint8_t readback[8 * 512];
    uint32_t bytes = count * 512;
    for (uint32_t i = 0; i < bytes; i++) {
        pattern[i] = (uint8_t)(i * 37 + seed);
        readback[i] = 0;
    }
    if (virtio_blk_write(sector, count, pattern) != 0) {
        kprintf("[blktest] sector=%lu count=%u write FAILED\n", sector, count);
        return -1;
    }
    if (virtio_blk_read(sector, count, readback) != 0) {
        kprintf("[blktest] sector=%lu count=%u read FAILED\n", sector, count);
        return -1;
    }
    for (uint32_t i = 0; i < bytes; i++) {
        if (pattern[i] != readback[i]) {
            kprintf("[blktest] sector=%lu count=%u MISMATCH at byte %u: wrote %u read %u\n",
                    sector, count, i, pattern[i], readback[i]);
            return -1;
        }
    }
    return 0;
}

static void blk_self_test(void) {
    if (blk_self_test_one(0, 1, 11) != 0) return;
    if (blk_self_test_one(1000, 1, 77) != 0) return;
    if (blk_self_test_one(5000, 8, 200) != 0) return;
    kprintf("[blktest] PASS\n");
}

void kmain(void) {
    arch_console_init();
    arch_fpu_boot_init();
    kputs("");
    kputs("Robu Kernel 0.9 x86_64 booting...");
    uint32_t boot_magic;
    if (arch_boot_magic(&boot_magic) == 0) {
        kprintf("[boot] robu_kernel -- loader magic 0x%x (valid), cores=%u\n",
                boot_magic, (unsigned)MAX_CPUS);
    } else {
        kprintf("[boot] robu_kernel -- loader magic unavailable, cores=%u\n",
                (unsigned)MAX_CPUS);
    }
    paddr_t mem_base;
    uint64_t mem_len;
    arch_detect_memory(&mem_base, &mem_len);
    rootfs_init();

    paddr_t mod_base = 0;
    uint64_t mod_len = 0;
    arch_boot_module(&mod_base, &mod_len);
    quiet_mode = cmdline_get("quiet") != NULL;
    uint64_t effective_len = mem_len;
    if (mem_base < ROBU_IDENTITY_MAP_LIMIT) {
        uint64_t max_len = ROBU_IDENTITY_MAP_LIMIT - mem_base;
        if (effective_len > max_len) {
            effective_len = max_len;
        }
    } else {
        effective_len = 0;
    }
    if (cmdline_get("force_fatal")) {
        effective_len = 0;
    }
    if (effective_len < UNTYPED_REGION_SIZE + DMA_REGION_SIZE + ACPI_HEAP_REGION_SIZE) {
        kprintf("[boot] FATAL: not enough identity-mapped RAM for the untyped+DMA+ACPI heap regions "
                "(have %lu bytes, need %lu)\n",
                effective_len, (uint64_t)(UNTYPED_REGION_SIZE + DMA_REGION_SIZE + ACPI_HEAP_REGION_SIZE));
        for (;;) { asm volatile("cli; hlt"); }
    }
    uint64_t pmm_len = effective_len - UNTYPED_REGION_SIZE - DMA_REGION_SIZE - ACPI_HEAP_REGION_SIZE;
    paddr_t dma_region_base = mem_base + pmm_len;
    paddr_t untyped_base = dma_region_base + DMA_REGION_SIZE;
    paddr_t acpi_heap_base = untyped_base + UNTYPED_REGION_SIZE;
    pmm_init(mem_base, pmm_len, mod_base, mod_len);
    dma_region_init(dma_region_base, DMA_REGION_SIZE);
    untyped_init(untyped_base, UNTYPED_REGION_SIZE);
    kheap_init(acpi_heap_base, ACPI_HEAP_REGION_SIZE);
    kprintf("[boot] dma region: %lu bytes at 0x%lx\n", (uint64_t)DMA_REGION_SIZE, dma_region_base);
    kprintf("[boot] untyped region: %lu bytes at 0x%lx\n", (uint64_t)UNTYPED_REGION_SIZE, untyped_base);
    kprintf("[boot] acpi heap region: %lu bytes at 0x%lx\n", (uint64_t)ACPI_HEAP_REGION_SIZE, acpi_heap_base);
    kprintf("[boot] frames: total=%lu free=%lu\n",
            pmm_stats.total_frames, pmm_stats.free_frames);

    rootfs_load_module();

    arch_gdt_init();
    arch_intr_init();
    vm_init();
    lapic_init();
    pci_log_devices();
    virtio_blk_init();
    if (virtio_blk_present() && cmdline_get("blktest")) {
        blk_self_test();
    }
    arch_timer_calibrate();
    arch_timer_percpu_init();
    kinfo_init(lapic_id(), 2);
    sig_trampoline_init();
    extern uint8_t kstack_top[];
    percpu_init_this_cpu(0, lapic_id(), kstack_top);
    kprintf("\033[33m[smp]\033[0m BSP cpu_id=0 apic_id=%u\n", lapic_id());
    smp_start_ap();
    sched_init();

    pager_init();
    thread_create("monitor", monitor_entry, stack_monitor + STACK_SIZE, 14);
    test_report_init();
    thread_create("console-in", console_in_entry, stack_console_in + STACK_SIZE, 20);
    root_task_init(untyped_base, UNTYPED_REGION_SIZE);
    devfs_init();
    ramfs_init();
    procfs_init();
    sysfs_init();
    bootfs_init();
    diskfs_init();
    bench_init();
    abitest_init(untyped_base, UNTYPED_REGION_SIZE);
    argvtest_init();
    panic_test_init();
    test_exit_init();
    toybox_cmd_init("true", 9);
    toybox_cmd_init("false", 9);
    toybox_cmd_init("pwd", 9);
    toybox_cmd_init("touch", 9);
    toybox_cmd_init_arg("cat", 9, "/dev/null");
    toybox_cmd_init_arg("tail", 9, "/dev/null");
    toybox_cmd_init("file", 9);
    toybox_cmd_init("find", 9);
    toybox_cmd_init("cp", 9);
    toybox_cmd_init("mv", 9);
    toybox_cmd_init("ls", 9);
    libctest_init();
    ramfstest_init();
    spawntest_init();
    sigtest_init();
    consoletest_init();
    readlinetest_init();
    toybox_sh_c_init();

    scheduler_ready = 1;
    sched_start();
}
