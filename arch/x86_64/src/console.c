#include "robu/types.h"
#include "robu/kprintf.h"
#include "robu/spinlock.h"
#include "robu/framebuffer.h"
#include "portio.h"

#define COM1 0x3F8

static int serial_present;

static void serial_init(void) {
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x80);
    outb(COM1 + 0, 0x01);
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03);
    outb(COM1 + 2, 0xC7);
    outb(COM1 + 4, 0x0B);
    outb(COM1 + 7, 0xAA);
    serial_present = (inb(COM1 + 7) == 0xAA);
    if (serial_present) {
        outb(COM1 + 7, 0x55);
        serial_present = (inb(COM1 + 7) == 0x55);
    }
}

#define SERIAL_READY_SPIN_MAX 100000

static void serial_putc(char c) {
    if (!serial_present) {
        return;
    }
    uint32_t spins = 0;
    while (!(inb(COM1 + 5) & 0x20)) {
        if (++spins >= SERIAL_READY_SPIN_MAX) {
            return;
        }
    }
    outb(COM1, (uint8_t)c);
}

#define VT_COUNT 6
#define CONSOLE_RING_SIZE 256

typedef struct {
    int raw_mode;
    char ring[CONSOLE_RING_SIZE];
    uint32_t ring_head, ring_tail;
} vt_state_t;

static vt_state_t vts[VT_COUNT];
static int active_vt = 0;

#define VGA_COLS 80
#define VGA_ROWS 25
#define VGA_HISTORY_MAX 200
#define DEFAULT_ATTR 0x07

static volatile uint16_t *const vga_mem = (volatile uint16_t *)0xB8000;

typedef struct {
    uint16_t live_screen[VGA_ROWS][VGA_COLS];
    uint16_t history_buf[VGA_HISTORY_MAX][VGA_COLS];
    int live_row;
    int live_col;
    int history_head;
    int view_offset;
    uint8_t vga_attr;
    int ansi_state;
    int ansi_param;
} vga_vt_state_t;

static vga_vt_state_t vga_vts[VT_COUNT];

static void update_cursor(void) {
    vga_vt_state_t *g = &vga_vts[active_vt];
    if (g->view_offset == 0) {
        uint16_t pos = (uint16_t)(g->live_row * VGA_COLS + g->live_col);
        outb(0x3D4, 0x0F); outb(0x3D5, (uint8_t)(pos & 0xFF));
        outb(0x3D4, 0x0E); outb(0x3D5, (uint8_t)((pos >> 8) & 0xFF));
    }
}

static void render_screen(void) {
    vga_vt_state_t *g = &vga_vts[active_vt];
    if (g->view_offset == 0) {
        volatile uint128_t *dst128 = (volatile uint128_t *)vga_mem;
        const uint128_t *src128 = (const uint128_t *)g->live_screen;
        for (int i = 0; i < (VGA_ROWS * VGA_COLS * 2) / 16; i++) {
            dst128[i] = src128[i];
        }
        update_cursor();
        return;
    }

    int total_history = g->history_head < VGA_HISTORY_MAX ? g->history_head : VGA_HISTORY_MAX;
    if (g->view_offset > total_history) g->view_offset = total_history;
    if (g->view_offset < 0) g->view_offset = 0;

    for (int r = 0; r < VGA_ROWS; r++) {
        int target_line = (total_history + r) - g->view_offset;
        volatile uint128_t *dst128 = (volatile uint128_t *)(vga_mem + r * VGA_COLS);

        if (target_line < 0) {
            uint16_t blank[VGA_COLS] __attribute__((aligned(16)));
            for (int c = 0; c < VGA_COLS; c++) blank[c] = (uint16_t)((g->vga_attr << 8) | ' ');
            const uint128_t *b128 = (const uint128_t *)blank;
            for (int i = 0; i < (VGA_COLS * 2) / 16; i++) dst128[i] = b128[i];
        } else if (target_line < total_history) {
            int hist_idx = (g->history_head - total_history + target_line + VGA_HISTORY_MAX) % VGA_HISTORY_MAX;
            const uint128_t *src128 = (const uint128_t *)g->history_buf[hist_idx];
            for (int i = 0; i < (VGA_COLS * 2) / 16; i++) dst128[i] = src128[i];
        } else {
            int live_r = target_line - total_history;
            if (live_r < VGA_ROWS) {
                const uint128_t *src128 = (const uint128_t *)g->live_screen[live_r];
                for (int i = 0; i < (VGA_COLS * 2) / 16; i++) dst128[i] = src128[i];
            } else {
                uint16_t blank[VGA_COLS] __attribute__((aligned(16)));
                for (int c = 0; c < VGA_COLS; c++) blank[c] = (uint16_t)((g->vga_attr << 8) | ' ');
                const uint128_t *b128 = (const uint128_t *)blank;
                for (int i = 0; i < (VGA_COLS * 2) / 16; i++) dst128[i] = b128[i];
            }
        }
    }
}

void arch_console_flush(void) {
    render_screen();
}

static void live_scroll_up(int vt) {
    vga_vt_state_t *g = &vga_vts[vt];
    int hist_idx = g->history_head % VGA_HISTORY_MAX;
    for (int c = 0; c < VGA_COLS; c++) {
        g->history_buf[hist_idx][c] = g->live_screen[0][c];
    }
    g->history_head++;

    for (int r = 1; r < VGA_ROWS; r++) {
        for (int c = 0; c < VGA_COLS; c++) {
            g->live_screen[r - 1][c] = g->live_screen[r][c];
        }
    }
    for (int c = 0; c < VGA_COLS; c++) {
        g->live_screen[VGA_ROWS - 1][c] = (uint16_t)((g->vga_attr << 8) | ' ');
    }
    g->live_row = VGA_ROWS - 1;
}

static void vga_clear_vt(int vt) {
    vga_vt_state_t *g = &vga_vts[vt];
    g->vga_attr = DEFAULT_ATTR;
    for (int r = 0; r < VGA_ROWS; r++) {
        for (int c = 0; c < VGA_COLS; c++) {
            g->live_screen[r][c] = (uint16_t)((g->vga_attr << 8) | ' ');
        }
    }
    for (int r = 0; r < VGA_HISTORY_MAX; r++) {
        for (int c = 0; c < VGA_COLS; c++) {
            g->history_buf[r][c] = (uint16_t)((g->vga_attr << 8) | ' ');
        }
    }
    g->history_head = 0;
    g->live_row = 0;
    g->live_col = 0;
    g->view_offset = 0;
}

static void apply_ansi_color(int vt, int code) {
    vga_vt_state_t *g = &vga_vts[vt];
    switch (code) {
    case 0:  g->vga_attr = DEFAULT_ATTR; break;
    case 1:  g->vga_attr |= 0x08; break;

    case 30: g->vga_attr = (g->vga_attr & 0xF0) | 0x00; break;
    case 31: g->vga_attr = (g->vga_attr & 0xF0) | 0x0C; break;
    case 32: g->vga_attr = (g->vga_attr & 0xF0) | 0x0A; break;
    case 33: g->vga_attr = (g->vga_attr & 0xF0) | 0x0E; break;
    case 34: g->vga_attr = (g->vga_attr & 0xF0) | 0x09; break;
    case 35: g->vga_attr = (g->vga_attr & 0xF0) | 0x0D; break;
    case 36: g->vga_attr = (g->vga_attr & 0xF0) | 0x0B; break;
    case 37: g->vga_attr = (g->vga_attr & 0xF0) | 0x0F; break;
    case 39: g->vga_attr = (g->vga_attr & 0xF0) | 0x07; break;

    case 40: g->vga_attr = (g->vga_attr & 0x8F) | 0x00; break;
    case 41: g->vga_attr = (g->vga_attr & 0x8F) | 0x40; break;
    case 42: g->vga_attr = (g->vga_attr & 0x8F) | 0x20; break;
    case 43: g->vga_attr = (g->vga_attr & 0x8F) | 0x60; break;
    case 44: g->vga_attr = (g->vga_attr & 0x8F) | 0x10; break;
    case 45: g->vga_attr = (g->vga_attr & 0x8F) | 0x50; break;
    case 46: g->vga_attr = (g->vga_attr & 0x8F) | 0x30; break;
    case 47: g->vga_attr = (g->vga_attr & 0x8F) | 0x70; break;
    case 49: g->vga_attr = (g->vga_attr & 0x8F) | 0x00; break;
    default: break;
    }
}

static void vga_putc(int vt, char c) {
    vga_vt_state_t *g = &vga_vts[vt];
    if (g->ansi_state == 0) {
        if (c == '\033') { g->ansi_state = 1; return; }
    } else if (g->ansi_state == 1) {
        if (c == '[') { g->ansi_state = 2; g->ansi_param = 0; return; }
        g->ansi_state = 0;
    } else if (g->ansi_state == 2) {
        if (c >= '0' && c <= '9') { g->ansi_param = g->ansi_param * 10 + (c - '0'); return; }
        if (c == ';' || c == 'm') {
            apply_ansi_color(vt, g->ansi_param);
            g->ansi_param = 0;
            if (c == 'm') g->ansi_state = 0;
            return;
        }
        if (c == 'K') {
            for (int col = g->live_col; col < VGA_COLS; col++) {
                g->live_screen[g->live_row][col] = (uint16_t)((g->vga_attr << 8) | ' ');
            }
            g->ansi_state = 0;
            return;
        }
        if (c == 'J' || c == 'H') {
            if (c == 'J') vga_clear_vt(vt);
            else if (c == 'H') { g->live_row = 0; g->live_col = 0; }
            g->ansi_state = 0;
            return;
        }
        g->ansi_state = 0;
        return;
    }

    if (c == '\n') {
        g->live_col = 0;
        if (++g->live_row >= VGA_ROWS) {
            live_scroll_up(vt);
        }
        return;
    }
    if (c == '\r') {
        g->live_col = 0;
        return;
    }
    if (c == '\b') {
        if (g->live_col > 0) g->live_col--;
        g->live_screen[g->live_row][g->live_col] = (uint16_t)((g->vga_attr << 8) | ' ');
        return;
    }
    if ((uint8_t)c < 0x20) {
        return;
    }

    g->live_screen[g->live_row][g->live_col] = (uint16_t)((g->vga_attr << 8) | (uint8_t)c);

    if (++g->live_col >= VGA_COLS) {
        g->live_col = 0;
        if (++g->live_row >= VGA_ROWS) {
            live_scroll_up(vt);
        }
    }
}

void arch_console_switch_vt(int vt) {
    if (vt < 0 || vt >= VT_COUNT || vt == active_vt) {
        return;
    }
    active_vt = vt;
    render_screen();
    fbconsole_clear();
}

int arch_console_get_active_vt(void) {
    return active_vt;
}

void arch_console_scroll(int delta) {
    vga_vt_state_t *g = &vga_vts[active_vt];
    g->view_offset += delta;
    if (g->view_offset < 0) {
        g->view_offset = 0;
    }
    render_screen();
}

void arch_console_init(void) {
    serial_init();
    for (int vt = 0; vt < VT_COUNT; vt++) {
        vga_clear_vt(vt);
    }
    render_screen();
}

void arch_console_putc(char c) {
    vga_putc(active_vt, c);
    fbconsole_putc(c);
    if (c == '\n') {
        serial_putc('\r');
    }
    serial_putc(c);
}

static spinlock_t console_ring_lock = SPINLOCK_INIT;

void arch_console_vt_putc(int vt, char c) {
    vga_putc(vt, c);
    if (vt == active_vt) {
        fbconsole_putc(c);
    }
    if (c == '\n') {
        serial_putc('\r');
    }
    serial_putc(c);
}

static void ring_push_locked(vt_state_t *v, char c) {
    uint32_t next = (v->ring_head + 1) % CONSOLE_RING_SIZE;
    if (next == v->ring_tail) {
        return;
    }
    v->ring[v->ring_head] = c;
    v->ring_head = next;
}

void arch_console_set_raw_mode(int vt, int enable) {
    if (vt < 0 || vt >= VT_COUNT) {
        return;
    }
    vts[vt].raw_mode = enable ? 1 : 0;
    if (!enable) {
        vga_vts[vt].vga_attr = DEFAULT_ATTR;
    }
}

int arch_console_get_raw_mode(int vt) {
    if (vt < 0 || vt >= VT_COUNT) {
        return 0;
    }
    return vts[vt].raw_mode;
}

void arch_console_feed_bytes(int vt, const uint8_t *buf, int len) {
    if (vt < 0 || vt >= VT_COUNT) {
        return;
    }
    vt_state_t *v = &vts[vt];
    spin_lock(&console_ring_lock);
    for (int i = 0; i < len; i++) {
        ring_push_locked(v, (char)buf[i]);
    }
    spin_unlock(&console_ring_lock);
    if (vt == active_vt && vga_vts[vt].view_offset != 0) {
        vga_vts[vt].view_offset = 0;
        render_screen();
    }
}

int64_t arch_console_port_io(uint16_t port, int width, int is_write, uint64_t value) {
    int allowed = (port == 0x60 || port == 0x64 || (port >= COM1 && port <= COM1 + 7));
    if (!allowed) {
        return -1;
    }
    if (is_write) {
        switch (width) {
        case 1: outb(port, (uint8_t)value); return 0;
        case 2: outw(port, (uint16_t)value); return 0;
        case 4: outl(port, (uint32_t)value); return 0;
        default: return -1;
        }
    }
    switch (width) {
    case 1: return (int64_t)inb(port);
    case 2: return (int64_t)inw(port);
    case 4: return (int64_t)inl(port);
    default: return -1;
    }
}

int arch_console_read_line_bytes(int vt, uint8_t *out, int max) {
    if (vt < 0 || vt >= VT_COUNT) {
        return 0;
    }
    vt_state_t *v = &vts[vt];
    spin_lock(&console_ring_lock);
    int n = 0;
    while (n < max && v->ring_tail != v->ring_head) {
        out[n++] = (uint8_t)v->ring[v->ring_tail];
        v->ring_tail = (v->ring_tail + 1) % CONSOLE_RING_SIZE;
    }
    spin_unlock(&console_ring_lock);
    return n;
}
