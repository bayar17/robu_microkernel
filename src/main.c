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
#include "robu/framebuffer.h"
#include "robu/random.h"
#include "lapic.h"
#include "smp.h"
#include "percpu.h"
#include "boot.h"

#define UNTYPED_REGION_SIZE (32u * 1024 * 1024)
#define DMA_REGION_SIZE (8u * 1024 * 1024)
#define ACPI_HEAP_REGION_SIZE (2u * 1024 * 1024)

int quiet_mode;

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

    paddr_t reserve_bases[PMM_MAX_RESERVED_REGIONS];
    uint64_t reserve_lens[PMM_MAX_RESERVED_REGIONS];
    int nreserved = arch_boot_module_count();
    if (nreserved > PMM_MAX_RESERVED_REGIONS) {
        nreserved = PMM_MAX_RESERVED_REGIONS;
    }
    if (nreserved > 0) {
        for (int i = 0; i < nreserved; i++) {
            arch_boot_module_at(i, &reserve_bases[i], &reserve_lens[i]);
        }
    } else {
        reserve_bases[0] = mod_base;
        reserve_lens[0] = mod_len;
        nreserved = 1;
    }

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
    pmm_init(mem_base, pmm_len, reserve_bases, reserve_lens, nreserved);
    extern paddr_t boot_pml4;
    fb_info_t fb;
    int have_fb = (arch_boot_framebuffer(&fb) == 0 && fb.type == 1);
    if (!have_fb) {
        have_fb = (vbe_set_mode(1024, 768, 32, &fb) == 0);
    }
    if (have_fb) {
        arch_vm_enable_framebuffer_write_combining();
        uint64_t fb_size = (uint64_t)fb.pitch * fb.height;
        uint64_t page_count = (fb_size + PAGE_SIZE_4K - 1) / PAGE_SIZE_4K;
        for (uint64_t i = 0; i < page_count; i++) {
            arch_vm_map_framebuffer_page((paddr_t)&boot_pml4, FRAMEBUFFER_VA + i * PAGE_SIZE_4K,
                                         (paddr_t)(fb.phys_addr + i * PAGE_SIZE_4K));
        }
        fbconsole_init(&fb);
        kprintf("[boot] framebuffer: phys=0x%lx virt=0x%lx %ux%u pitch=%u bpp=%u type=%u\n",
                fb.phys_addr, (uint64_t)FRAMEBUFFER_VA, fb.width, fb.height, fb.pitch, fb.bpp, fb.type);
    } else {
        kprintf("[boot] framebuffer: not available\n");
    }
    dma_region_init(dma_region_base, DMA_REGION_SIZE);
    untyped_init(untyped_base, UNTYPED_REGION_SIZE);
    kheap_init(acpi_heap_base, ACPI_HEAP_REGION_SIZE);
    kprintf("[boot] dma region: %lu bytes at 0x%lx\n", (uint64_t)DMA_REGION_SIZE, dma_region_base);
    kprintf("[boot] untyped region: %lu bytes at 0x%lx\n", (uint64_t)UNTYPED_REGION_SIZE, untyped_base);
    kprintf("[boot] acpi heap region: %lu bytes at 0x%lx\n", (uint64_t)ACPI_HEAP_REGION_SIZE, acpi_heap_base);
    kprintf("[boot] frames: total=%lu free=%lu\n",
            pmm_stats.total_frames, pmm_stats.free_frames);

    arch_gdt_init();
    arch_intr_init();
    vm_init();
    lapic_init();
    arch_timer_calibrate();
    arch_timer_percpu_init();
    random_init();
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
    root_task_init(untyped_base, UNTYPED_REGION_SIZE);
    devfs_init();
    console_driver_init();
    ext2fs_init();
    procfs_init();
    sysfs_init();
    kprintf("[boot] starting blockdrv\n");
    blockdrv_init();
    kprintf("[boot] starting diskfs\n");
    diskfs_init();

    testing_set_untyped(untyped_base, UNTYPED_REGION_SIZE);

    kprintf("[boot] starting scheduler\n");
    scheduler_ready = 1;
    sched_start();
}
