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
int arch_console_getc(void);
void arch_console_flush(void);
void arch_console_line_feed(int c);
int arch_console_read_line_bytes(int vt, uint8_t *out, int max);
void arch_console_set_raw_mode(int vt, int enable);
int arch_console_get_raw_mode(int vt);
void arch_console_switch_vt(int vt);
int arch_console_get_active_vt(void);

#define SYS_INFO_CAT_CONSOLE_MODE 10
#define SYS_INFO_CAT_ACTIVE_VT 16
#endif
