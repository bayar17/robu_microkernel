#ifndef ROBU_KPRINTF_H
#define ROBU_KPRINTF_H
#include "robu/types.h"

void kprintf(const char *fmt, ...);
void kputs(const char *s);
extern int quiet_mode;
void kwrite(const uint8_t *buf, uint64_t len);

void arch_console_init(void);
void arch_console_putc(char c);
int arch_console_getc(void);
void arch_console_flush(void);
void arch_console_line_feed(int c);
int arch_console_read_line_bytes(uint8_t *out, int max);
void arch_console_set_raw_mode(int enable);
int arch_console_get_raw_mode(void);

#define SYS_INFO_CAT_CONSOLE_MODE 10
#endif
