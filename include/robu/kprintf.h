#ifndef ROBU_KPRINTF_H
#define ROBU_KPRINTF_H
#include "robu/types.h"

void kprintf(const char *fmt, ...);
void kputs(const char *s);
extern int quiet_mode;
void kwrite(int vt, const uint8_t *buf, uint64_t len);

void arch_console_init(void);
void arch_console_putc(char c);
void arch_console_vt_putc(int vt, char c);
void arch_console_flush(void);
void arch_console_feed_bytes(int vt, const uint8_t *buf, int len);
int arch_console_read_line_bytes(int vt, uint8_t *out, int max);
void arch_console_set_raw_mode(int vt, int enable);
int arch_console_get_raw_mode(int vt);
void arch_console_switch_vt(int vt);
int arch_console_get_active_vt(void);
void arch_console_scroll(int delta);
int64_t arch_console_port_io(uint16_t port, int width, int is_write, uint64_t value);
void arch_console_mouse_feed(uint64_t packed_event);
int arch_console_mouse_read(uint64_t *out, int max);
void arch_console_set_fb_owner(tid_t tid);
tid_t arch_console_get_fb_owner(void);

#define SYS_INFO_CAT_CONSOLE_MODE 10
#define SYS_INFO_CAT_ACTIVE_VT 16
#define SYS_INFO_CAT_PORT_IO 17
#define SYS_INFO_CAT_CONSOLE_FEED 18
#define SYS_INFO_CAT_CONSOLE_SIGNAL_FG 19
#define SYS_INFO_CAT_SET_ACTIVE_VT 20
#define SYS_INFO_CAT_CONSOLE_SCROLL 21
#define SYS_INFO_CAT_MOUSE_FEED 22
#define SYS_INFO_CAT_MOUSE_READ 23
#define SYS_INFO_CAT_FB_MAP 24
#define SYS_INFO_CAT_SHM_GET 25
#define SYS_INFO_CAT_SHM_AT  26
#define SYS_INFO_CAT_SHM_DT  27
#define SYS_INFO_CAT_SHM_CTL 28
#define SYS_INFO_CAT_SOCK_CREATE  29
#define SYS_INFO_CAT_SOCK_BIND    30
#define SYS_INFO_CAT_SOCK_LISTEN  31
#define SYS_INFO_CAT_SOCK_CONNECT 32
#define SYS_INFO_CAT_SOCK_ACCEPT  33
#define SYS_INFO_CAT_SOCK_READ    34
#define SYS_INFO_CAT_SOCK_WRITE   35
#define SYS_INFO_CAT_SOCK_CLOSE   36
#endif
