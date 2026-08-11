#include "robu/types.h"
#include "robu/arch.h"
#include "portio.h"
#include "uacpi_glue.h"

struct idt_entry {
    uint16_t off_lo;
    uint16_t sel;
    uint8_t ist;
    uint8_t type_attr;
    uint16_t off_mid;
    uint32_t off_hi;
    uint32_t reserved;
} __attribute__((packed));

struct idt_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

#define IDT_VECTORS_USED 52
static struct idt_entry idt[256] __attribute__((aligned(16)));
extern uint64_t trap_stub_table[IDT_VECTORS_USED];

static void idt_set_gate(int vec, uint64_t handler, uint8_t dpl) {
    idt[vec].off_lo = handler & 0xFFFF;
    idt[vec].sel = 0x08;
    idt[vec].ist = 0;
    idt[vec].type_attr = (uint8_t)(0x8E | (dpl << 5));
    idt[vec].off_mid = (handler >> 16) & 0xFFFF;
    idt[vec].off_hi = (uint32_t)(handler >> 32);
    idt[vec].reserved = 0;
}

#define PIC1_CMD  0x20
#define PIC1_DATA 0x21
#define PIC2_CMD  0xA0
#define PIC2_DATA 0xA1

static void pic_init(void) {
    outb(PIC1_CMD, 0x11); io_wait();
    outb(PIC2_CMD, 0x11); io_wait();
    outb(PIC1_DATA, 32); io_wait();
    outb(PIC2_DATA, 40); io_wait();
    outb(PIC1_DATA, 4); io_wait();
    outb(PIC2_DATA, 2); io_wait();
    outb(PIC1_DATA, 0x01); io_wait();
    outb(PIC2_DATA, 0x01); io_wait();
    outb(PIC1_DATA, 0xFF);
    outb(PIC2_DATA, 0xFF);
}

#define MAX_DYNAMIC_IRQ 16
static arch_irq_handler_t irq_handlers[MAX_DYNAMIC_IRQ];
static void *irq_ctxs[MAX_DYNAMIC_IRQ];

int arch_irq_register(uint32_t irq, arch_irq_handler_t handler, void *ctx) {
    if (irq >= MAX_DYNAMIC_IRQ) {
        return -1;
    }
    irq_handlers[irq] = handler;
    irq_ctxs[irq] = ctx;
    uint16_t port = irq < 8 ? PIC1_DATA : PIC2_DATA;
    uint8_t bit = irq < 8 ? (uint8_t)irq : (uint8_t)(irq - 8);
    uint8_t mask = inb(port);
    outb(port, mask & (uint8_t)~(1u << bit));
    if (irq >= 8) {
        uint8_t cascade_mask = inb(PIC1_DATA);
        outb(PIC1_DATA, cascade_mask & (uint8_t)~(1u << 2));
    }
    return 0;
}

void arch_irq_unregister(uint32_t irq) {
    if (irq >= MAX_DYNAMIC_IRQ) {
        return;
    }
    irq_handlers[irq] = 0;
    irq_ctxs[irq] = 0;
    uint16_t port = irq < 8 ? PIC1_DATA : PIC2_DATA;
    uint8_t bit = irq < 8 ? (uint8_t)irq : (uint8_t)(irq - 8);
    uint8_t mask = inb(port);
    outb(port, mask | (uint8_t)(1u << bit));
}

int arch_irq_dispatch(uint32_t vector) {
    if (vector < 32 || vector >= 48) {
        return 0;
    }
    uint32_t irq = vector - 32;
    if (irq >= MAX_DYNAMIC_IRQ || !irq_handlers[irq]) {
        return 0;
    }
    irq_handlers[irq](irq_ctxs[irq]);
    outb(PIC1_CMD, 0x20);
    if (irq >= 8) {
        outb(PIC2_CMD, 0x20);
    }
    return 1;
}

static struct idt_ptr shared_idt_ptr;

void arch_intr_init(void) {
    for (int v = 0; v < IDT_VECTORS_USED; v++) {
        idt_set_gate(v, trap_stub_table[v], v == 0x30 ? 3 : 0);
    }
    shared_idt_ptr.limit = sizeof(idt) - 1;
    shared_idt_ptr.base = (uint64_t)idt;
    asm volatile("lidt %0" : : "m"(shared_idt_ptr));
    pic_init();
}

void arch_intr_init_ap(void) {
    asm volatile("lidt %0" : : "m"(shared_idt_ptr));
}

void arch_idle(void) {
    asm volatile("hlt");
}

void arch_test_exit(int code) {
    outb(0xF4, (uint8_t)code);
    for (;;) {
        asm volatile("cli; hlt");
    }
}

void arch_reboot(void) {
    uacpi_glue_reboot();
    uint8_t good = 0x02;
    while (good & 0x02) good = inb(0x64);
    outb(0x64, 0xFE);
    outb(0xCF9, 0x0E);
    outb(0xCF9, 0x06);
    for (;;) {
        asm volatile("cli; hlt");
    }
}

void arch_halt(void) {
    for (;;) {
        asm volatile("cli; hlt");
    }
}

void arch_shutdown(void) {
    uacpi_glue_shutdown();
    for (;;) {
        asm volatile("cli; hlt");
    }
}

uint64_t arch_irq_save(void) {
    uint64_t flags;
    asm volatile("pushfq; pop %0; cli" : "=r"(flags) : : "memory");
    return flags;
}

void arch_irq_restore(uint64_t flags) {
    asm volatile("push %0; popfq" : : "r"(flags) : "memory", "cc");
}
