#include <uacpi/kernel_api.h>
#include <uacpi/status.h>
#include <uacpi/uacpi.h>
#include <uacpi/sleep.h>
#include "robu/types.h"
#include "robu/arch.h"
#include "robu/kprintf.h"
#include "robu/kheap.h"
#include "robu/spinlock.h"
#include "robu/vm.h"
#include "robu/pmm.h"
#include "percpu.h"
#include "portio.h"
#include "pci.h"
#include "uacpi_glue.h"

extern paddr_t boot_pml4;

uacpi_status uacpi_kernel_get_rsdp(uacpi_phys_addr *out_rsdp_address) {
    paddr_t rsdp;
    if (arch_boot_rsdp(&rsdp) != 0) {
        return UACPI_STATUS_NOT_FOUND;
    }
    *out_rsdp_address = (uacpi_phys_addr)rsdp;
    return UACPI_STATUS_OK;
}

void *uacpi_kernel_map(uacpi_phys_addr addr, uacpi_size len) {
    if (addr + len <= ROBU_IDENTITY_MAP_LIMIT) {
        return (void *)addr;
    }
    paddr_t page_base = addr & ~(PAGE_SIZE_4K - 1);
    uint64_t offset = addr - page_base;
    uint64_t map_len = (offset + len + PAGE_SIZE_4K - 1) & ~(PAGE_SIZE_4K - 1);
    for (uint64_t off = 0; off < map_len; off += PAGE_SIZE_4K) {
        arch_vm_map_page((paddr_t)&boot_pml4, page_base + off, page_base + off,
                          VM_PROT_READ | VM_PROT_WRITE);
    }
    return (void *)(page_base + offset);
}

void uacpi_kernel_unmap(void *addr, uacpi_size len) {
    paddr_t start = (paddr_t)addr;
    if (start + len <= ROBU_IDENTITY_MAP_LIMIT) {
        return;
    }
    paddr_t page_base = start & ~(PAGE_SIZE_4K - 1);
    uint64_t offset = start - page_base;
    uint64_t map_len = (offset + len + PAGE_SIZE_4K - 1) & ~(PAGE_SIZE_4K - 1);
    for (uint64_t off = 0; off < map_len; off += PAGE_SIZE_4K) {
        arch_vm_unmap_page((paddr_t)&boot_pml4, page_base + off);
    }
}

void uacpi_kernel_log(uacpi_log_level level, const uacpi_char *msg) {
    const char *prefix;
    switch (level) {
    case UACPI_LOG_ERROR:
        prefix = "[uacpi] ERROR: ";
        break;
    case UACPI_LOG_WARN:
        prefix = "[uacpi] WARN: ";
        break;
    case UACPI_LOG_INFO:
        prefix = "[uacpi] ";
        break;
    case UACPI_LOG_TRACE:
        prefix = "[uacpi] TRACE: ";
        break;
    default:
        prefix = "[uacpi] DEBUG: ";
        break;
    }
    kprintf("%s%s", prefix, msg);
}

typedef struct {
    pci_addr_t addr;
} uacpi_pci_handle_t;

uacpi_status uacpi_kernel_pci_device_open(uacpi_pci_address address, uacpi_handle *out_handle) {
    uacpi_pci_handle_t *h = kmalloc(sizeof(*h));
    if (!h) {
        return UACPI_STATUS_OUT_OF_MEMORY;
    }
    h->addr.bus = (uint8_t)address.bus;
    h->addr.device = (uint8_t)address.device;
    h->addr.function = (uint8_t)address.function;
    *out_handle = h;
    return UACPI_STATUS_OK;
}

void uacpi_kernel_pci_device_close(uacpi_handle handle) {
    kfree(handle);
}

uacpi_status uacpi_kernel_pci_read8(uacpi_handle handle, uacpi_size offset, uacpi_u8 *value) {
    uacpi_pci_handle_t *h = handle;
    *value = pci_cfg_read8(h->addr, (uint8_t)offset);
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_pci_read16(uacpi_handle handle, uacpi_size offset, uacpi_u16 *value) {
    uacpi_pci_handle_t *h = handle;
    *value = pci_cfg_read16(h->addr, (uint8_t)offset);
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_pci_read32(uacpi_handle handle, uacpi_size offset, uacpi_u32 *value) {
    uacpi_pci_handle_t *h = handle;
    *value = pci_cfg_read32(h->addr, (uint8_t)offset);
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_pci_write8(uacpi_handle handle, uacpi_size offset, uacpi_u8 value) {
    uacpi_pci_handle_t *h = handle;
    pci_cfg_write8(h->addr, (uint8_t)offset, value);
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_pci_write16(uacpi_handle handle, uacpi_size offset, uacpi_u16 value) {
    uacpi_pci_handle_t *h = handle;
    pci_cfg_write16(h->addr, (uint8_t)offset, value);
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_pci_write32(uacpi_handle handle, uacpi_size offset, uacpi_u32 value) {
    uacpi_pci_handle_t *h = handle;
    pci_cfg_write32(h->addr, (uint8_t)offset, value);
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_io_map(uacpi_io_addr base, uacpi_size len, uacpi_handle *out_handle) {
    (void)len;
    *out_handle = (uacpi_handle)base;
    return UACPI_STATUS_OK;
}

void uacpi_kernel_io_unmap(uacpi_handle handle) {
    (void)handle;
}

uacpi_status uacpi_kernel_io_read8(uacpi_handle handle, uacpi_size offset, uacpi_u8 *out_value) {
    *out_value = inb((uint16_t)((uint64_t)handle + offset));
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_io_read16(uacpi_handle handle, uacpi_size offset, uacpi_u16 *out_value) {
    *out_value = inw((uint16_t)((uint64_t)handle + offset));
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_io_read32(uacpi_handle handle, uacpi_size offset, uacpi_u32 *out_value) {
    *out_value = inl((uint16_t)((uint64_t)handle + offset));
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_io_write8(uacpi_handle handle, uacpi_size offset, uacpi_u8 in_value) {
    outb((uint16_t)((uint64_t)handle + offset), in_value);
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_io_write16(uacpi_handle handle, uacpi_size offset, uacpi_u16 in_value) {
    outw((uint16_t)((uint64_t)handle + offset), in_value);
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_io_write32(uacpi_handle handle, uacpi_size offset, uacpi_u32 in_value) {
    outl((uint16_t)((uint64_t)handle + offset), in_value);
    return UACPI_STATUS_OK;
}

void *uacpi_kernel_alloc(uacpi_size size) {
    return kmalloc(size);
}

void uacpi_kernel_free(void *mem) {
    kfree(mem);
}

#define UACPI_TIMING_CAL_WINDOW_MS 50u
#define UACPI_EVENT_INFINITE_CAP_MS 5000u

static uint64_t uacpi_tsc_hz;
static uint64_t uacpi_tsc_boot;

static uint64_t uacpi_rdtsc(void) {
    uint32_t lo, hi;
    asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static void uacpi_timing_calibrate(void) {
    if (uacpi_tsc_hz) {
        return;
    }
    uint32_t divisor = (1193182u / 1000u) * UACPI_TIMING_CAL_WINDOW_MS;
    uint8_t port61 = inb(0x61);
    outb(0x61, port61 & (uint8_t)~0x03);
    outb(0x43, 0xB0);
    outb(0x42, divisor & 0xFF);
    outb(0x42, (divisor >> 8) & 0xFF);
    outb(0x61, (port61 & (uint8_t)~0x02) | 0x01);
    uint64_t start = uacpi_rdtsc();
    while (!(inb(0x61) & 0x20)) {
    }
    uint64_t end = uacpi_rdtsc();
    uacpi_tsc_hz = (end - start) * 1000ULL / UACPI_TIMING_CAL_WINDOW_MS;
    uacpi_tsc_boot = end;
    kprintf("[uacpi] tsc calibrated: %lu Hz\n", uacpi_tsc_hz);
}

static uint64_t uacpi_scale(uint64_t value, uint64_t mul, uint64_t div) {
    uint64_t whole = (value / div) * mul;
    uint64_t rem = (value % div) * mul / div;
    return whole + rem;
}

static uint64_t uacpi_ms_to_ticks(uint64_t ms) {
    return uacpi_scale(ms, uacpi_tsc_hz, 1000ULL);
}

uacpi_u64 uacpi_kernel_get_nanoseconds_since_boot(void) {
    uacpi_timing_calibrate();
    uint64_t delta = uacpi_rdtsc() - uacpi_tsc_boot;
    return uacpi_scale(delta, 1000000000ULL, uacpi_tsc_hz);
}

void uacpi_kernel_stall(uacpi_u8 usec) {
    uacpi_timing_calibrate();
    uint64_t ticks = uacpi_scale(usec, uacpi_tsc_hz, 1000000ULL);
    uint64_t start = uacpi_rdtsc();
    while (uacpi_rdtsc() - start < ticks) {
        asm volatile("pause");
    }
}

void uacpi_kernel_sleep(uacpi_u64 msec) {
    uacpi_timing_calibrate();
    uint64_t ticks = uacpi_ms_to_ticks(msec);
    uint64_t start = uacpi_rdtsc();
    while (uacpi_rdtsc() - start < ticks) {
        asm volatile("pause");
    }
}

typedef struct {
    spinlock_t lock;
} uacpi_mutex_t;

uacpi_handle uacpi_kernel_create_mutex(void) {
    return kmalloc_zeroed(sizeof(uacpi_mutex_t));
}

void uacpi_kernel_free_mutex(uacpi_handle handle) {
    kfree(handle);
}

uacpi_status uacpi_kernel_acquire_mutex(uacpi_handle handle, uacpi_u16 timeout) {
    uacpi_mutex_t *m = handle;
    if (timeout == 0) {
        return spin_trylock(&m->lock) ? UACPI_STATUS_OK : UACPI_STATUS_TIMEOUT;
    }
    uacpi_timing_calibrate();
    uint64_t ms = (timeout == 0xFFFF) ? UACPI_EVENT_INFINITE_CAP_MS : timeout;
    uint64_t ticks = uacpi_ms_to_ticks(ms);
    uint64_t start = uacpi_rdtsc();
    while (!spin_trylock(&m->lock)) {
        if (uacpi_rdtsc() - start >= ticks) {
            return UACPI_STATUS_TIMEOUT;
        }
        asm volatile("pause");
    }
    return UACPI_STATUS_OK;
}

void uacpi_kernel_release_mutex(uacpi_handle handle) {
    uacpi_mutex_t *m = handle;
    spin_unlock(&m->lock);
}

typedef struct {
    spinlock_t lock;
    int32_t count;
} uacpi_event_t;

uacpi_handle uacpi_kernel_create_event(void) {
    return kmalloc_zeroed(sizeof(uacpi_event_t));
}

void uacpi_kernel_free_event(uacpi_handle handle) {
    kfree(handle);
}

uacpi_bool uacpi_kernel_wait_for_event(uacpi_handle handle, uacpi_u16 timeout) {
    uacpi_event_t *e = handle;
    uacpi_timing_calibrate();
    uint64_t ms = (timeout == 0xFFFF) ? UACPI_EVENT_INFINITE_CAP_MS : timeout;
    uint64_t ticks = uacpi_ms_to_ticks(ms);
    uint64_t start = uacpi_rdtsc();
    for (;;) {
        spin_lock(&e->lock);
        if (e->count > 0) {
            e->count--;
            spin_unlock(&e->lock);
            return UACPI_TRUE;
        }
        spin_unlock(&e->lock);
        if (timeout == 0 || uacpi_rdtsc() - start >= ticks) {
            return UACPI_FALSE;
        }
        asm volatile("pause");
    }
}

void uacpi_kernel_signal_event(uacpi_handle handle) {
    uacpi_event_t *e = handle;
    spin_lock(&e->lock);
    e->count++;
    spin_unlock(&e->lock);
}

void uacpi_kernel_reset_event(uacpi_handle handle) {
    uacpi_event_t *e = handle;
    spin_lock(&e->lock);
    e->count = 0;
    spin_unlock(&e->lock);
}

uacpi_thread_id uacpi_kernel_get_thread_id(void) {
    return (uacpi_thread_id)(uint64_t)(this_cpu()->cpu_id + 1);
}

uacpi_interrupt_state uacpi_kernel_disable_interrupts(void) {
    return (uacpi_interrupt_state)arch_irq_save();
}

void uacpi_kernel_restore_interrupts(uacpi_interrupt_state state) {
    arch_irq_restore((uint64_t)state);
}

uacpi_status uacpi_kernel_handle_firmware_request(uacpi_firmware_request *req) {
    if (req->type == UACPI_FIRMWARE_REQUEST_TYPE_FATAL) {
        kprintf("[uacpi] AML Fatal: type=%u code=%u arg=0x%lx\n",
                req->fatal.type, req->fatal.code, req->fatal.arg);
    } else {
        kprintf("[uacpi] AML Breakpoint hit\n");
    }
    return UACPI_STATUS_OK;
}

static uacpi_interrupt_handler uacpi_sci_handler;
static uacpi_handle uacpi_sci_ctx;
static uint32_t uacpi_sci_irq;
static int uacpi_sci_installed;

static void uacpi_sci_trampoline(void *ctx) {
    (void)ctx;
    if (uacpi_sci_handler) {
        uacpi_sci_handler(uacpi_sci_ctx);
    }
}

uacpi_status uacpi_kernel_install_interrupt_handler(
    uacpi_u32 irq, uacpi_interrupt_handler handler, uacpi_handle ctx,
    uacpi_handle *out_irq_handle) {
    if (uacpi_sci_installed) {
        return UACPI_STATUS_ALREADY_EXISTS;
    }
    if (arch_irq_register(irq, uacpi_sci_trampoline, UACPI_NULL) != 0) {
        return UACPI_STATUS_INVALID_ARGUMENT;
    }
    uacpi_sci_handler = handler;
    uacpi_sci_ctx = ctx;
    uacpi_sci_irq = irq;
    uacpi_sci_installed = 1;
    *out_irq_handle = (uacpi_handle)(uint64_t)(irq + 1);
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_uninstall_interrupt_handler(
    uacpi_interrupt_handler handler, uacpi_handle irq_handle) {
    (void)handler;
    (void)irq_handle;
    if (uacpi_sci_installed) {
        arch_irq_unregister(uacpi_sci_irq);
        uacpi_sci_installed = 0;
        uacpi_sci_handler = UACPI_NULL;
    }
    return UACPI_STATUS_OK;
}

uacpi_handle uacpi_kernel_create_spinlock(void) {
    return kmalloc_zeroed(sizeof(spinlock_t));
}

void uacpi_kernel_free_spinlock(uacpi_handle handle) {
    kfree(handle);
}

uacpi_cpu_flags uacpi_kernel_lock_spinlock(uacpi_handle handle) {
    uint64_t flags = arch_irq_save();
    spin_lock((spinlock_t *)handle);
    return (uacpi_cpu_flags)flags;
}

void uacpi_kernel_unlock_spinlock(uacpi_handle handle, uacpi_cpu_flags flags) {
    spin_unlock((spinlock_t *)handle);
    arch_irq_restore((uint64_t)flags);
}

uacpi_status uacpi_kernel_schedule_work(
    uacpi_work_type type, uacpi_work_handler handler, uacpi_handle ctx) {
    (void)type;
    handler(ctx);
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_wait_for_work_completion(void) {
    return UACPI_STATUS_OK;
}

#define UACPI_EARLY_TABLE_BUF_SIZE 4096
static uint8_t uacpi_early_table_buf[UACPI_EARLY_TABLE_BUF_SIZE];
static int uacpi_subsystem_ready;

static int uacpi_ensure_subsystem(void) {
    if (uacpi_subsystem_ready) {
        return 0;
    }
    uacpi_status ret = uacpi_setup_early_table_access(uacpi_early_table_buf,
                                                       sizeof(uacpi_early_table_buf));
    if (uacpi_unlikely_error(ret)) {
        kprintf("[uacpi] early table access setup failed: %s\n", uacpi_status_to_string(ret));
        return -1;
    }
    ret = uacpi_initialize(0);
    if (uacpi_unlikely_error(ret)) {
        kprintf("[uacpi] uacpi_initialize failed: %s\n", uacpi_status_to_string(ret));
        return -1;
    }
    uacpi_subsystem_ready = 1;
    return 0;
}

int uacpi_glue_shutdown(void) {
    if (uacpi_ensure_subsystem() != 0) {
        return -1;
    }
    uacpi_status ret = uacpi_namespace_load();
    if (uacpi_unlikely_error(ret)) {
        kprintf("[uacpi] namespace load failed: %s\n", uacpi_status_to_string(ret));
        return -1;
    }
    ret = uacpi_namespace_initialize();
    if (uacpi_unlikely_error(ret)) {
        kprintf("[uacpi] namespace initialize failed: %s\n", uacpi_status_to_string(ret));
        return -1;
    }
    ret = uacpi_prepare_for_sleep_state(UACPI_SLEEP_STATE_S5);
    if (uacpi_unlikely_error(ret)) {
        kprintf("[uacpi] prepare_for_sleep_state(S5) failed: %s\n", uacpi_status_to_string(ret));
        return -1;
    }
    uint64_t flags = arch_irq_save();
    ret = uacpi_enter_sleep_state(UACPI_SLEEP_STATE_S5);
    arch_irq_restore(flags);
    if (uacpi_unlikely_error(ret)) {
        kprintf("[uacpi] enter_sleep_state(S5) failed: %s\n", uacpi_status_to_string(ret));
        return -1;
    }
    return 0;
}

int uacpi_glue_reboot(void) {
    if (uacpi_ensure_subsystem() != 0) {
        return -1;
    }
    uacpi_status ret = uacpi_reboot();
    if (uacpi_unlikely_error(ret)) {
        kprintf("[uacpi] uacpi_reboot failed: %s\n", uacpi_status_to_string(ret));
        return -1;
    }
    return 0;
}
