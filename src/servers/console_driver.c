#include "robu/types.h"
#include "robu/uipc.h"

#define VT_COUNT 6
#define COM1 0x3F8
#define CONSOLE_LINE_MAX 128
#define SIGINT 2

#define SYS_INFO_CAT_CONSOLE_MODE 10
#define SYS_INFO_CAT_ACTIVE_VT 16
#define SYS_INFO_CAT_PORT_IO 17
#define SYS_INFO_CAT_CONSOLE_FEED 18
#define SYS_INFO_CAT_CONSOLE_SIGNAL_FG 19
#define SYS_INFO_CAT_SET_ACTIVE_VT 20
#define SYS_INFO_CAT_CONSOLE_SCROLL 21

static int port_io_read(uint16_t port, int width) {
    msg_regs_t m = (msg_regs_t){0};
    m.word[0] = SYS_INFO_CAT_PORT_IO;
    m.word[1] = port;
    m.word[2] = (uint64_t)width;
    m.word[3] = 0;
    robu_ipc_raw(0, 0, IPC_FLAG_SYS_INFO, &m, NULL);
    return (int)(int64_t)m.word[0];
}

static void port_io_write(uint16_t port, int width, uint64_t value) {
    msg_regs_t m = (msg_regs_t){0};
    m.word[0] = SYS_INFO_CAT_PORT_IO;
    m.word[1] = port;
    m.word[2] = (uint64_t)width;
    m.word[3] = 1;
    m.word[4] = value;
    robu_ipc_raw(0, 0, IPC_FLAG_SYS_INFO, &m, NULL);
}

static int serial_present(void) {
    port_io_write(COM1 + 7, 1, 0xAA);
    if (port_io_read(COM1 + 7, 1) != 0xAA) {
        return 0;
    }
    port_io_write(COM1 + 7, 1, 0x55);
    if (port_io_read(COM1 + 7, 1) != 0x55) {
        return 0;
    }
    return 1;
}

static void console_feed(int vt, const uint8_t *buf, int len) {
    int off = 0;
    while (off < len) {
        int chunk = (len - off) > 32 ? 32 : (len - off);
        msg_regs_t m = (msg_regs_t){0};
        m.word[0] = SYS_INFO_CAT_CONSOLE_FEED;
        m.word[1] = (uint64_t)chunk | ((uint64_t)vt << 8);
        uint64_t words[4] = {0, 0, 0, 0};
        uint8_t *bytes = (uint8_t *)words;
        for (int i = 0; i < chunk; i++) {
            bytes[i] = buf[off + i];
        }
        m.word[2] = words[0];
        m.word[3] = words[1];
        m.word[4] = words[2];
        m.word[5] = words[3];
        robu_ipc_raw(0, 0, IPC_FLAG_SYS_INFO, &m, NULL);
        off += chunk;
    }
}

static void console_signal_fg(int vt, int signum) {
    msg_regs_t m = (msg_regs_t){0};
    m.word[0] = SYS_INFO_CAT_CONSOLE_SIGNAL_FG;
    m.word[1] = (uint64_t)vt;
    m.word[2] = (uint64_t)signum;
    robu_ipc_raw(0, 0, IPC_FLAG_SYS_INFO, &m, NULL);
}

static void set_active_vt(int vt) {
    msg_regs_t m = (msg_regs_t){0};
    m.word[0] = SYS_INFO_CAT_SET_ACTIVE_VT;
    m.word[1] = (uint64_t)vt;
    robu_ipc_raw(0, 0, IPC_FLAG_SYS_INFO, &m, NULL);
}

static void console_scroll(int delta) {
    msg_regs_t m = (msg_regs_t){0};
    m.word[0] = SYS_INFO_CAT_CONSOLE_SCROLL;
    m.word[1] = (uint64_t)(int64_t)delta;
    robu_ipc_raw(0, 0, IPC_FLAG_SYS_INFO, &m, NULL);
}

static int get_raw_mode(int vt) {
    msg_regs_t m = (msg_regs_t){0};
    m.word[0] = SYS_INFO_CAT_CONSOLE_MODE;
    m.word[1] = 2;
    m.word[2] = (uint64_t)vt;
    robu_ipc_raw(0, 0, IPC_FLAG_SYS_INFO, &m, NULL);
    return (int)m.word[0];
}

static void console_write(int vt, const char *buf, int len) {
    if (len > 40) {
        len = 40;
    }
    msg_regs_t m = (msg_regs_t){0};
    m.word[0] = (uint64_t)len | ((uint64_t)vt << 8);
    uint64_t words[5] = {0, 0, 0, 0, 0};
    uint8_t *bytes = (uint8_t *)words;
    for (int i = 0; i < len; i++) {
        bytes[i] = (uint8_t)buf[i];
    }
    m.word[1] = words[0];
    m.word[2] = words[1];
    m.word[3] = words[2];
    m.word[4] = words[3];
    m.word[5] = words[4];
    robu_ipc_raw(0, 0, IPC_FLAG_CONSOLE_WRITE, &m, NULL);
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
static int ctrl_pressed = 0;
static int alt_pressed = 0;
static int extended_pending = 0;
static char pending_bytes[4];
static int pending_len = 0;
static int pending_pos = 0;
static int g_active_vt = 0;

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
    if (!(port_io_read(0x64, 1) & 0x01)) {
        return -1;
    }
    uint8_t scancode = (uint8_t)port_io_read(0x60, 1);
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
    if (scancode == 0x1D) {
        extended_pending = 0;
        ctrl_pressed = 1;
        return -1;
    }
    if (scancode == 0x9D) {
        extended_pending = 0;
        ctrl_pressed = 0;
        return -1;
    }
    if (scancode == 0x38) {
        extended_pending = 0;
        alt_pressed = 1;
        return -1;
    }
    if (scancode == 0xB8) {
        extended_pending = 0;
        alt_pressed = 0;
        return -1;
    }
    if (ctrl_pressed && alt_pressed && scancode >= 0x3B && scancode <= 0x40) {
        g_active_vt = (int)(scancode - 0x3B);
        set_active_vt(g_active_vt);
        return -1;
    }
    if (scancode & 0x80) {
        extended_pending = 0;
        return -1;
    }
    if (extended_pending) {
        extended_pending = 0;

        if (scancode == 0x49 || (shift_pressed && scancode == 0x48)) {
            console_scroll(5);
            return -1;
        }
        if (scancode == 0x51 || (shift_pressed && scancode == 0x50)) {
            console_scroll(-5);
            return -1;
        }

        if (get_raw_mode(g_active_vt) && extended_key_to_ansi(scancode)) {
            pending_pos = 1;
            return (int)(uint8_t)pending_bytes[0];
        }
        return -1;
    }

    if (scancode < 128) {
        char c = shift_pressed ? scancode_ascii_shift[scancode] : scancode_ascii[scancode];
        if (c) return (int)(uint8_t)c;
    }
    return -1;
}

static int serial_getc(void) {
    if (!(port_io_read(COM1 + 5, 1) & 0x01)) {
        return -1;
    }
    return port_io_read(COM1, 1);
}

static int g_serial_present = -1;

static int getc_raw(void) {
    if (g_serial_present < 0) {
        g_serial_present = serial_present();
    }
    int c = g_serial_present ? serial_getc() : -1;
    if (c >= 0) {
        return c;
    }
    return ps2_getc();
}

static char line_buf[VT_COUNT][CONSOLE_LINE_MAX];
static int line_len[VT_COUNT];

static void line_feed(int c) {
    int vt = g_active_vt;
    if (c == 0x03) {
        line_len[vt] = 0;
        console_write(vt, "^C\n", 3);
        uint8_t b = 0x03;
        console_feed(vt, &b, 1);
        console_signal_fg(vt, SIGINT);
        return;
    }
    if (get_raw_mode(vt)) {
        uint8_t b = (uint8_t)c;
        console_feed(vt, &b, 1);
        return;
    }
    if (c == '\b' || c == 0x7F) {
        if (line_len[vt] > 0) {
            line_len[vt]--;
            console_write(vt, "\b \b", 3);
        }
        return;
    }
    if (c == '\r' || c == '\n') {
        console_write(vt, "\n", 1);
        int n = line_len[vt];
        uint8_t tmp[CONSOLE_LINE_MAX + 1];
        for (int i = 0; i < n; i++) {
            tmp[i] = (uint8_t)line_buf[vt][i];
        }
        tmp[n] = '\n';
        console_feed(vt, tmp, n + 1);
        line_len[vt] = 0;
        return;
    }
    if (c == 0x04) {
        int n = line_len[vt];
        if (n > 0) {
            uint8_t tmp[CONSOLE_LINE_MAX];
            for (int i = 0; i < n; i++) {
                tmp[i] = (uint8_t)line_buf[vt][i];
            }
            console_feed(vt, tmp, n);
        } else {
            uint8_t b = 0x04;
            console_feed(vt, &b, 1);
        }
        console_write(vt, "\n", 1);
        line_len[vt] = 0;
        return;
    }
    if (line_len[vt] < CONSOLE_LINE_MAX - 1) {
        line_buf[vt][line_len[vt]++] = (char)c;
        char b = (char)c;
        console_write(vt, &b, 1);
    }
}

void _start(void) {
    for (;;) {
        int c;
        while ((c = getc_raw()) >= 0) {
            line_feed(c);
        }
        ipc_sleep(1);
    }
}
