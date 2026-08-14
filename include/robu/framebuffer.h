#ifndef ROBU_FRAMEBUFFER_H
#define ROBU_FRAMEBUFFER_H
#include "robu/types.h"
#define FRAMEBUFFER_VA 0x0000000088000000ULL
#define FRAMEBUFFER_USER_VA 0x0000000100000000ULL
typedef struct {
    uint64_t phys_addr;
    uint32_t pitch;
    uint32_t width;
    uint32_t height;
    uint8_t bpp;
    uint8_t type;
    uint8_t red_pos, red_size;
    uint8_t green_pos, green_size;
    uint8_t blue_pos, blue_size;
} fb_info_t;
extern fb_info_t g_boot_fb;
extern int g_boot_fb_present;
int arch_boot_framebuffer(fb_info_t *out);
int vbe_set_mode(uint32_t width, uint32_t height, uint32_t bpp, fb_info_t *out);
void fbconsole_init(const fb_info_t *fb);
void fbconsole_putc(char c);
void fbconsole_clear(void);
#endif
