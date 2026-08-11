#include "robu/types.h"
#include "robu/kprintf.h"
#include "robu/spinlock.h"
#include "portio.h"

#define COM1 0x3F8

static void serial_init(void) {
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x80);
    outb(COM1 + 0, 0x01);
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03);
    outb(COM1 + 2, 0xC7);
    outb(COM1 + 4, 0x0B);
}

static void serial_putc(char c) {
    while (!(inb(COM1 + 5) & 0x20)) {}
    outb(COM1, (uint8_t)c);
}

static int serial_getc(void) {
    if (!(inb(COM1 + 5) & 0x01)) {
        return -1;
    }
    return (int)(uint8_t)inb(COM1);
}

static const char scancode_ascii[128] = {
    0,  27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
  '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
    0,  'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
    0, '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/',   0,
  '*',   0, ' ',   0
};

static const char scancode_ascii_shift[128] = {
    0,  27, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
  '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
    0,  'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~',
    0, '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?',   0,
  '*',   0, ' ',   0
};

static int shift_pressed = 0;
static int extended_pending = 0;
static int console_raw_mode = 0;
static char pending_bytes[4];
static int pending_len = 0;
static int pending_pos = 0;

#define VGA_COLS 80
#define VGA_ROWS 25
#define VGA_HISTORY_MAX 200
#define DEFAULT_ATTR 0x07

static volatile uint16_t *const vga_mem = (volatile uint16_t *)0xB8000;

static uint16_t live_screen[VGA_ROWS][VGA_COLS] __attribute__((aligned(16)));
static uint16_t history_buf[VGA_HISTORY_MAX][VGA_COLS] __attribute__((aligned(16)));

static int live_row = 0;
static int live_col = 0;
static int history_head = 0; 
static int view_offset = 0;

static uint8_t vga_attr = DEFAULT_ATTR;
static int ansi_state = 0;
static int ansi_param = 0;

static void update_cursor(void) {
    if (view_offset == 0) {
        uint16_t pos = (uint16_t)(live_row * VGA_COLS + live_col);
        outb(0x3D4, 0x0F); outb(0x3D5, (uint8_t)(pos & 0xFF));
        outb(0x3D4, 0x0E); outb(0x3D5, (uint8_t)((pos >> 8) & 0xFF));
    }
}

static void render_screen(void) {
    if (view_offset == 0) {
        volatile uint128_t *dst128 = (volatile uint128_t *)vga_mem;
        const uint128_t *src128 = (const uint128_t *)live_screen;
        for (int i = 0; i < (VGA_ROWS * VGA_COLS * 2) / 16; i++) {
            dst128[i] = src128[i];
        }
        update_cursor();
        return;
    }

    int total_history = history_head < VGA_HISTORY_MAX ? history_head : VGA_HISTORY_MAX;
    if (view_offset > total_history) view_offset = total_history;
    if (view_offset < 0) view_offset = 0;

    for (int r = 0; r < VGA_ROWS; r++) {
        int target_line = (total_history + r) - view_offset;
        volatile uint128_t *dst128 = (volatile uint128_t *)(vga_mem + r * VGA_COLS);

        if (target_line < 0) {
            uint16_t blank[VGA_COLS] __attribute__((aligned(16)));
            for (int c = 0; c < VGA_COLS; c++) blank[c] = (uint16_t)((vga_attr << 8) | ' ');
            const uint128_t *b128 = (const uint128_t *)blank;
            for (int i = 0; i < (VGA_COLS * 2) / 16; i++) dst128[i] = b128[i];
        } else if (target_line < total_history) {
            int hist_idx = (history_head - total_history + target_line + VGA_HISTORY_MAX) % VGA_HISTORY_MAX;
            const uint128_t *src128 = (const uint128_t *)history_buf[hist_idx];
            for (int i = 0; i < (VGA_COLS * 2) / 16; i++) dst128[i] = src128[i];
        } else {
            int live_r = target_line - total_history;
            if (live_r < VGA_ROWS) {
                const uint128_t *src128 = (const uint128_t *)live_screen[live_r];
                for (int i = 0; i < (VGA_COLS * 2) / 16; i++) dst128[i] = src128[i];
            } else {
                uint16_t blank[VGA_COLS] __attribute__((aligned(16)));
                for (int c = 0; c < VGA_COLS; c++) blank[c] = (uint16_t)((vga_attr << 8) | ' ');
                const uint128_t *b128 = (const uint128_t *)blank;
                for (int i = 0; i < (VGA_COLS * 2) / 16; i++) dst128[i] = b128[i];
            }
        }
    }
}

void arch_console_flush(void) {
    render_screen();
}

static void live_scroll_up(void) {
    int hist_idx = history_head % VGA_HISTORY_MAX;
    for (int c = 0; c < VGA_COLS; c++) {
        history_buf[hist_idx][c] = live_screen[0][c];
    }
    history_head++;

    for (int r = 1; r < VGA_ROWS; r++) {
        for (int c = 0; c < VGA_COLS; c++) {
            live_screen[r - 1][c] = live_screen[r][c];
        }
    }
    for (int c = 0; c < VGA_COLS; c++) {
        live_screen[VGA_ROWS - 1][c] = (uint16_t)((vga_attr << 8) | ' ');
    }
    live_row = VGA_ROWS - 1;
}

static void vga_clear(void) {
    vga_attr = DEFAULT_ATTR;
    for (int r = 0; r < VGA_ROWS; r++) {
        for (int c = 0; c < VGA_COLS; c++) {
            live_screen[r][c] = (uint16_t)((vga_attr << 8) | ' ');
        }
    }
    for (int r = 0; r < VGA_HISTORY_MAX; r++) {
        for (int c = 0; c < VGA_COLS; c++) {
            history_buf[r][c] = (uint16_t)((vga_attr << 8) | ' ');
        }
    }
    history_head = 0;
    live_row = 0;
    live_col = 0;
    view_offset = 0;
    render_screen();
}

static void apply_ansi_color(int code) {
    switch (code) {
    case 0:  vga_attr = DEFAULT_ATTR; break;
    case 1:  vga_attr |= 0x08; break;

    case 30: vga_attr = (vga_attr & 0xF0) | 0x00; break;
    case 31: vga_attr = (vga_attr & 0xF0) | 0x0C; break;
    case 32: vga_attr = (vga_attr & 0xF0) | 0x0A; break;
    case 33: vga_attr = (vga_attr & 0xF0) | 0x0E; break;
    case 34: vga_attr = (vga_attr & 0xF0) | 0x09; break;
    case 35: vga_attr = (vga_attr & 0xF0) | 0x0D; break;
    case 36: vga_attr = (vga_attr & 0xF0) | 0x0B; break;
    case 37: vga_attr = (vga_attr & 0xF0) | 0x0F; break;
    case 39: vga_attr = (vga_attr & 0xF0) | 0x07; break;

    case 40: vga_attr = (vga_attr & 0x8F) | 0x00; break;
    case 41: vga_attr = (vga_attr & 0x8F) | 0x40; break;
    case 42: vga_attr = (vga_attr & 0x8F) | 0x20; break;
    case 43: vga_attr = (vga_attr & 0x8F) | 0x60; break;
    case 44: vga_attr = (vga_attr & 0x8F) | 0x10; break;
    case 45: vga_attr = (vga_attr & 0x8F) | 0x50; break;
    case 46: vga_attr = (vga_attr & 0x8F) | 0x30; break;
    case 47: vga_attr = (vga_attr & 0x8F) | 0x70; break;
    case 49: vga_attr = (vga_attr & 0x8F) | 0x00; break;
    default: break;
    }
}

static void vga_putc(char c) {
    if (ansi_state == 0) {
        if (c == '\033') { ansi_state = 1; return; }
    } else if (ansi_state == 1) {
        if (c == '[') { ansi_state = 2; ansi_param = 0; return; }
        ansi_state = 0;
    } else if (ansi_state == 2) {
        if (c >= '0' && c <= '9') { ansi_param = ansi_param * 10 + (c - '0'); return; }
        if (c == ';' || c == 'm') {
            apply_ansi_color(ansi_param);
            ansi_param = 0;
            if (c == 'm') ansi_state = 0;
            return;
        }
        /* IMPLEMENTAÇÃO DE \033[K (Erase from Cursor to End of Line) */
        if (c == 'K') {
            for (int col = live_col; col < VGA_COLS; col++) {
                live_screen[live_row][col] = (uint16_t)((vga_attr << 8) | ' ');
            }
            ansi_state = 0;
            return;
        }
        if (c == 'J' || c == 'H') {
            if (c == 'J') vga_clear();
            else if (c == 'H') { live_row = 0; live_col = 0; }
            ansi_state = 0;
            return;
        }
        ansi_state = 0;
        return;
    }

    if (c == '\n') {
        live_col = 0;
        if (++live_row >= VGA_ROWS) {
            live_scroll_up();
        }
        return;
    }
    if (c == '\r') {
        live_col = 0;
        return;
    }
    if (c == '\b') {
        if (live_col > 0) live_col--;
        live_screen[live_row][live_col] = (uint16_t)((vga_attr << 8) | ' ');
        return;
    }

    live_screen[live_row][live_col] = (uint16_t)((vga_attr << 8) | (uint8_t)c);

    if (++live_col >= VGA_COLS) {
        live_col = 0;
        if (++live_row >= VGA_ROWS) {
            live_scroll_up();
        }
    }
}

static int extended_key_to_ansi(uint8_t scancode) {
    switch (scancode) {
    case 0x48: pending_bytes[0] = '\033'; pending_bytes[1] = '['; pending_bytes[2] = 'A';
               pending_len = 3; return 1;
    case 0x50: pending_bytes[0] = '\033'; pending_bytes[1] = '['; pending_bytes[2] = 'B';
               pending_len = 3; return 1;
    case 0x4D: pending_bytes[0] = '\033'; pending_bytes[1] = '['; pending_bytes[2] = 'C';
               pending_len = 3; return 1;
    case 0x4B: pending_bytes[0] = '\033'; pending_bytes[1] = '['; pending_bytes[2] = 'D';
               pending_len = 3; return 1;
    case 0x47: pending_bytes[0] = '\033'; pending_bytes[1] = '['; pending_bytes[2] = 'H';
               pending_len = 3; return 1;
    case 0x4F: pending_bytes[0] = '\033'; pending_bytes[1] = '['; pending_bytes[2] = 'F';
               pending_len = 3; return 1;
    case 0x53: pending_bytes[0] = '\033'; pending_bytes[1] = '['; pending_bytes[2] = '3';
               pending_bytes[3] = '~'; pending_len = 4; return 1;
    default: return 0;
    }
}

static int ps2_getc(void) {
    if (pending_pos < pending_len) {
        return (int)(uint8_t)pending_bytes[pending_pos++];
    }
    pending_len = 0;
    pending_pos = 0;
    if (!(inb(0x64) & 0x01)) {
        return -1;
    }
    uint8_t scancode = inb(0x60);
    if (scancode == 0xE0) {
        extended_pending = 1;
        return -1;
    }
    if (scancode == 0x2A || scancode == 0x36) {
        extended_pending = 0;
        shift_pressed = 1;
        return -1;
    }
    if (scancode == 0xAA || scancode == 0xB6) {
        extended_pending = 0;
        shift_pressed = 0;
        return -1;
    }
    if (scancode & 0x80) {
        extended_pending = 0;
        return -1;
    }
    if (extended_pending) {
        extended_pending = 0;

        if (scancode == 0x49 || (shift_pressed && scancode == 0x48)) {
            view_offset += 5;
            render_screen();
            return -1;
        }
        if (scancode == 0x51 || (shift_pressed && scancode == 0x50)) {
            view_offset -= 5;
            if (view_offset < 0) view_offset = 0;
            render_screen();
            return -1;
        }

        if (console_raw_mode && extended_key_to_ansi(scancode)) {
            pending_pos = 1;
            return (int)(uint8_t)pending_bytes[0];
        }
        return -1;
    }

    if (view_offset != 0) {
        view_offset = 0;
        render_screen();
    }

    if (scancode < 128) {
        char c = shift_pressed ? scancode_ascii_shift[scancode] : scancode_ascii[scancode];
        if (c) return (int)(uint8_t)c;
    }
    return -1;
}

void arch_console_init(void) {
    serial_init();
    vga_clear();
}

void arch_console_putc(char c) {
    if (c == '\n') {
        serial_putc('\r');
    }
    serial_putc(c);
    vga_putc(c);
}

int arch_console_getc(void) {
    int c = serial_getc();
    if (c >= 0) return c;
    return ps2_getc();
}

static spinlock_t console_ring_lock = SPINLOCK_INIT;
#define CONSOLE_LINE_MAX 128
#define CONSOLE_RING_SIZE 256
static char console_ring[CONSOLE_RING_SIZE];
static uint32_t ring_head, ring_tail;
static char line_buf[CONSOLE_LINE_MAX];
static int line_len;

static void ring_push_locked(char c) {
    uint32_t next = (ring_head + 1) % CONSOLE_RING_SIZE;
    if (next == ring_tail) {
        return;
    }
    console_ring[ring_head] = c;
    ring_head = next;
}

void arch_console_set_raw_mode(int enable) {
    console_raw_mode = enable ? 1 : 0;
    if (!enable) {
        vga_attr = DEFAULT_ATTR;
    }
}

int arch_console_get_raw_mode(void) {
    return console_raw_mode;
}

void arch_console_line_feed(int c) {
    if (console_raw_mode) {
        spin_lock(&console_ring_lock);
        ring_push_locked((char)c);
        spin_unlock(&console_ring_lock);
        return;
    }
    if (c == '\b' || c == 0x7F) {
        if (line_len > 0) {
            line_len--;
            arch_console_putc('\b');
            arch_console_putc(' ');
            arch_console_putc('\b');
        }
        return;
    }
    if (c == '\r' || c == '\n') {
        arch_console_putc('\n');
        spin_lock(&console_ring_lock);
        for (int i = 0; i < line_len; i++) {
            ring_push_locked(line_buf[i]);
        }
        ring_push_locked('\n');
        spin_unlock(&console_ring_lock);
        line_len = 0;
        return;
    }
    if (line_len < CONSOLE_LINE_MAX - 1) {
        line_buf[line_len++] = (char)c;
        arch_console_putc((char)c);
    }
}

int arch_console_read_line_bytes(uint8_t *out, int max) {
    spin_lock(&console_ring_lock);
    int n = 0;
    while (n < max && ring_tail != ring_head) {
        out[n++] = (uint8_t)console_ring[ring_tail];
        ring_tail = (ring_tail + 1) % CONSOLE_RING_SIZE;
    }
    spin_unlock(&console_ring_lock);
    return n;
}
