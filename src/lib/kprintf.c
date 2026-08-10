#include "robu/types.h"
#include "robu/kprintf.h"
#include "robu/spinlock.h"
#include "robu/arch.h"

static spinlock_t console_lock = SPINLOCK_INIT;
typedef __builtin_va_list va_list;
#define va_start(v, l) __builtin_va_start(v, l)
#define va_arg(v, t)   __builtin_va_arg(v, t)
#define va_end(v)      __builtin_va_end(v)

static void emit_udec(uint64_t v) {
    char buf[20];
    int i = 0;
    if (v == 0) {
        arch_console_putc('0');
        return;
    }
    while (v) {
        buf[i++] = '0' + (char)(v % 10);
        v /= 10;
    }
    while (i--) {
        arch_console_putc(buf[i]);
    }
}

static void emit_hex(uint64_t v, int min_digits) {
    static const char digits[] = "0123456789abcdef";
    char buf[16];
    int i = 0;
    while (v) {
        buf[i++] = digits[v & 0xF];
        v >>= 4;
    }
    if (i == 0) {
        buf[i++] = '0';
    }
    while (i < min_digits) {
        buf[i++] = '0';
    }
    while (i--) {
        arch_console_putc(buf[i]);
    }
}

static void emit_str(const char *s) {
    if (!s) {
        s = "(null)";
    }
    while (*s) {
        arch_console_putc(*s++);
    }
}

void kputs(const char *s) {
    uint64_t flags = arch_irq_save();
    spin_lock(&console_lock);
    emit_str(s);
    arch_console_putc('\n');
    arch_console_flush();
    spin_unlock(&console_lock);
    arch_irq_restore(flags);
}

void kwrite(const uint8_t *buf, uint64_t len) {
    uint64_t flags = arch_irq_save();
    spin_lock(&console_lock);
    for (uint64_t i = 0; i < len; i++) {
        arch_console_putc((char)buf[i]);
    }
    /* Atomic batch flush: transfers RAM framebuffer to MMIO 0xB8000 via 128-bit instructions */
    arch_console_flush();
    spin_unlock(&console_lock);
    arch_irq_restore(flags);
}

void kprintf(const char *fmt, ...) {
    uint64_t flags = arch_irq_save();
    spin_lock(&console_lock);
    va_list ap;
    va_start(ap, fmt);
    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            arch_console_putc(*fmt);
            continue;
        }
        fmt++;
        int wide = 0;
        while (*fmt == 'l') {
            wide = 1;
            fmt++;
        }
        switch (*fmt) {
        case 'c':
            arch_console_putc((char)va_arg(ap, int));
            break;
        case 's':
            emit_str(va_arg(ap, const char *));
            break;
        case 'd': {
            int64_t v = wide ? va_arg(ap, int64_t) : (int64_t)va_arg(ap, int32_t);
            if (v < 0) {
                arch_console_putc('-');
                v = -v;
            }
            emit_udec((uint64_t)v);
            break;
        }
        case 'u':
            emit_udec(wide ? va_arg(ap, uint64_t) : (uint64_t)va_arg(ap, uint32_t));
            break;
        case 'x':
            emit_hex(wide ? va_arg(ap, uint64_t) : (uint64_t)va_arg(ap, uint32_t), 1);
            break;
        case 'p':
            emit_str("0x");
            emit_hex((uint64_t)va_arg(ap, void *), 16);
            break;
        case '%':
            arch_console_putc('%');
            break;
        case '\0':
            va_end(ap);
            arch_console_flush();
            spin_unlock(&console_lock);
            arch_irq_restore(flags);
            return;
        default:
            arch_console_putc('%');
            arch_console_putc(*fmt);
            break;
        }
    }
    va_end(ap);
    arch_console_flush();
    spin_unlock(&console_lock);
    arch_irq_restore(flags);
}
