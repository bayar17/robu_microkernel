#include "robu/types.h"
#include "robu/framebuffer.h"
#include "robu/font8x16.h"
#include "robu/spinlock.h"

#define FB_CHAR_W 8
#define FB_CHAR_H 16
#define FB_COLOR_FG 0x00FFFFFFu
#define FB_COLOR_BG 0x00000000u
#define FB_CURSOR_H 2
#define FB_TEXT_MAX_COLS 512
#define FB_TEXT_MAX_ROWS 256

static volatile uint32_t *fb_mem;
static uint32_t fb_pitch_px;
static uint32_t fb_width, fb_height;
static uint32_t fb_cols, fb_rows;
static uint32_t cur_row, cur_col;
static int fb_ansi_state;
static uint32_t fb_ansi_param;
static uint8_t fb_cells[FB_TEXT_MAX_ROWS][FB_TEXT_MAX_COLS];
static spinlock_t fbconsole_lock = SPINLOCK_INIT;

fb_info_t g_boot_fb;
int g_boot_fb_present;

static void fb_draw_glyph(uint32_t col, uint32_t row, unsigned char c) {
    if (col >= fb_cols || row >= fb_rows) {
        return;
    }
    uint32_t x0 = col * FB_CHAR_W;
    uint32_t y0 = row * FB_CHAR_H;
    const unsigned char *glyph = &font8x16[(uint32_t)c * 16];
    for (uint32_t gy = 0; gy < FB_CHAR_H; gy++) {
        unsigned char bits = glyph[gy];
        volatile uint32_t *line = fb_mem + (y0 + gy) * fb_pitch_px + x0;
        for (uint32_t gx = 0; gx < FB_CHAR_W; gx++) {
            line[gx] = (bits & (0x01 << gx)) ? FB_COLOR_FG : FB_COLOR_BG;
        }
    }
}

static void fb_cursor_xor(void) {
    if (cur_col >= fb_cols || cur_row >= fb_rows) {
        return;
    }
    uint32_t x0 = cur_col * FB_CHAR_W;
    uint32_t y0 = cur_row * FB_CHAR_H + (FB_CHAR_H - FB_CURSOR_H);
    for (uint32_t gy = 0; gy < FB_CURSOR_H; gy++) {
        volatile uint32_t *line = fb_mem + (y0 + gy) * fb_pitch_px + x0;
        for (uint32_t gx = 0; gx < FB_CHAR_W; gx++) {
            line[gx] ^= FB_COLOR_FG;
        }
    }
}

static void fb_clear_cells(void) {
    for (uint32_t row = 0; row < fb_rows; row++) {
        for (uint32_t col = 0; col < fb_cols; col++) {
            fb_cells[row][col] = ' ';
        }
    }
}

static void fb_render_cells(void) {
    for (uint32_t row = 0; row < fb_rows; row++) {
        for (uint32_t col = 0; col < fb_cols; col++) {
            fb_draw_glyph(col, row, fb_cells[row][col]);
        }
    }
}

static void fb_scroll_up(void) {
    for (uint32_t row = 1; row < fb_rows; row++) {
        for (uint32_t col = 0; col < fb_cols; col++) {
            fb_cells[row - 1][col] = fb_cells[row][col];
        }
    }
    for (uint32_t col = 0; col < fb_cols; col++) {
        fb_cells[fb_rows - 1][col] = ' ';
    }
    fb_render_cells();
}

static void fb_next_row(void) {
    cur_col = 0;
    cur_row++;
    if (cur_row >= fb_rows) {
        fb_scroll_up();
        cur_row = fb_rows - 1;
    }
}

static void fb_clear_screen(void) {
    for (uint32_t y = 0; y < fb_height; y++) {
        volatile uint32_t *line = fb_mem + y * fb_pitch_px;
        for (uint32_t x = 0; x < fb_width; x++) {
            line[x] = FB_COLOR_BG;
        }
    }
}

void fbconsole_clear(void) {
    if (!fb_mem) {
        return;
    }
    spin_lock(&fbconsole_lock);
    cur_row = 0;
    cur_col = 0;
    fb_ansi_state = 0;
    fb_ansi_param = 0;
    fb_clear_cells();
    fb_clear_screen();
    fb_cursor_xor();
    spin_unlock(&fbconsole_lock);
}

void fbconsole_init(const fb_info_t *fb) {
    g_boot_fb = *fb;
    g_boot_fb_present = 1;
    fb_mem = (volatile uint32_t *)FRAMEBUFFER_VA;
    fb_pitch_px = fb->pitch / 4;
    fb_width = fb->width;
    fb_height = fb->height;
    fb_cols = fb->width / FB_CHAR_W;
    fb_rows = fb->height / FB_CHAR_H;
    if (fb_cols > FB_TEXT_MAX_COLS) {
        fb_cols = FB_TEXT_MAX_COLS;
    }
    if (fb_rows > FB_TEXT_MAX_ROWS) {
        fb_rows = FB_TEXT_MAX_ROWS;
    }
    fbconsole_clear();
}

void fbconsole_redraw(const uint16_t *cells, uint32_t cols, uint32_t rows,
                      uint32_t cursor_row, uint32_t cursor_col) {
    if (!fb_mem || !cells) {
        return;
    }
    spin_lock(&fbconsole_lock);
    fb_clear_cells();
    uint32_t source_cols = cols;
    if (cols > fb_cols) {
        cols = fb_cols;
    }
    if (rows > fb_rows) {
        rows = fb_rows;
    }
    for (uint32_t row = 0; row < rows; row++) {
        for (uint32_t col = 0; col < cols; col++) {
            fb_cells[row][col] = (uint8_t)cells[row * source_cols + col];
        }
    }
    cur_row = cursor_row < fb_rows ? cursor_row : fb_rows - 1;
    cur_col = cursor_col < fb_cols ? cursor_col : fb_cols - 1;
    fb_ansi_state = 0;
    fb_ansi_param = 0;
    fb_render_cells();
    fb_cursor_xor();
    spin_unlock(&fbconsole_lock);
}

void fbconsole_putc(char c) {
    if (!fb_mem) {
        return;
    }
    spin_lock(&fbconsole_lock);
    fb_cursor_xor();
    if (fb_ansi_state == 0) {
        if (c == '\033') {
            fb_ansi_state = 1;
            fb_cursor_xor();
            spin_unlock(&fbconsole_lock);
            return;
        }
    } else if (fb_ansi_state == 1) {
        if (c == '[') {
            fb_ansi_state = 2;
            fb_ansi_param = 0;
            fb_cursor_xor();
            spin_unlock(&fbconsole_lock);
            return;
        }
        fb_ansi_state = 0;
    } else if (fb_ansi_state == 2) {
        if (c >= '0' && c <= '9') {
            fb_ansi_param = fb_ansi_param * 10 + (uint32_t)(c - '0');
            fb_cursor_xor();
            spin_unlock(&fbconsole_lock);
            return;
        }
        if (c == ';') {
            fb_ansi_param = 0;
            fb_cursor_xor();
            spin_unlock(&fbconsole_lock);
            return;
        }
        if (c == 'H') {
            cur_row = 0;
            cur_col = 0;
        } else if (c == 'J' && fb_ansi_param != 3) {
            fb_clear_cells();
            fb_clear_screen();
        }
        fb_ansi_state = 0;
        fb_ansi_param = 0;
        fb_cursor_xor();
        spin_unlock(&fbconsole_lock);
        return;
    }
    if (c == '\n') {
        fb_next_row();
        fb_cursor_xor();
        spin_unlock(&fbconsole_lock);
        return;
    }
    if (c == '\r') {
        cur_col = 0;
        fb_cursor_xor();
        spin_unlock(&fbconsole_lock);
        return;
    }
    if (c == '\b') {
        if (cur_col > 0) {
            cur_col--;
            fb_cells[cur_row][cur_col] = ' ';
            fb_draw_glyph(cur_col, cur_row, ' ');
        }
        fb_cursor_xor();
        spin_unlock(&fbconsole_lock);
        return;
    }
    if ((unsigned char)c < 0x20) {
        fb_cursor_xor();
        spin_unlock(&fbconsole_lock);
        return;
    }
    fb_cells[cur_row][cur_col] = (uint8_t)c;
    fb_draw_glyph(cur_col, cur_row, (unsigned char)c);
    if (++cur_col >= fb_cols) {
        fb_next_row();
    }
    fb_cursor_xor();
    spin_unlock(&fbconsole_lock);
}
