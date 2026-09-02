typedef unsigned char uint8_t;
typedef unsigned short uint16_t;
typedef unsigned int uint32_t;
typedef unsigned long long uint64_t;
typedef __UINTPTR_TYPE__ uintptr_t;

#include "mbi_tags.h"

#define E820_ENTRY_SIZE 20

static void u32_set(uint8_t *buf, int off, uint32_t v) {
    buf[off] = (uint8_t)v;
    buf[off + 1] = (uint8_t)(v >> 8);
    buf[off + 2] = (uint8_t)(v >> 16);
    buf[off + 3] = (uint8_t)(v >> 24);
}

static void u64_set(uint8_t *buf, int off, uint64_t v) {
    for (int i = 0; i < 8; i++) {
        buf[off + i] = (uint8_t)(v >> (8 * i));
    }
}

static uint32_t u32_get(const uint8_t *buf, int off) {
    return (uint32_t)buf[off] | ((uint32_t)buf[off + 1] << 8) |
           ((uint32_t)buf[off + 2] << 16) | ((uint32_t)buf[off + 3] << 24);
}

unsigned int align8(unsigned int x) {
    return (x + 7) & ~7u;
}

unsigned int append_cmdline_tag(unsigned int buf, unsigned int off, const char *cmdline) {
    uint8_t *b = (uint8_t *)(unsigned long long)buf;
    uint32_t cmdline_len = 0;
    while (cmdline[cmdline_len]) {
        cmdline_len++;
    }
    uint32_t tag_size = 8 + cmdline_len + 1;
    u32_set(b, (int)off, 1);
    u32_set(b, (int)off + 4, tag_size);
    for (uint32_t i = 0; i <= cmdline_len; i++) {
        b[off + 8 + i] = (uint8_t)cmdline[i];
    }
    return off + align8(tag_size);
}

unsigned int append_module_tag(unsigned int buf, unsigned int off, unsigned int mod_start,
                                unsigned int mod_end, const char *mod_name) {
    uint8_t *b = (uint8_t *)(unsigned long long)buf;
    uint32_t mod_name_len = 0;
    while (mod_name[mod_name_len]) {
        mod_name_len++;
    }
    uint32_t tag_size = 16 + mod_name_len + 1;
    u32_set(b, (int)off, 3);
    u32_set(b, (int)off + 4, tag_size);
    u32_set(b, (int)off + 8, mod_start);
    u32_set(b, (int)off + 12, mod_end);
    for (uint32_t i = 0; i <= mod_name_len; i++) {
        b[off + 16 + i] = (uint8_t)mod_name[i];
    }
    return off + align8(tag_size);
}

unsigned int append_mmap_tag(unsigned int buf, unsigned int off, uintptr_t e820_raw,
                              unsigned int e820_count) {
    uint8_t *b = (uint8_t *)(unsigned long long)buf;
    const uint8_t *raw = (const uint8_t *)e820_raw;
    uint32_t tag_size = 16 + e820_count * 24;
    u32_set(b, (int)off, 6);
    u32_set(b, (int)off + 4, tag_size);
    u32_set(b, (int)off + 8, 24);
    u32_set(b, (int)off + 12, 0);
    for (uint32_t i = 0; i < e820_count; i++) {
        const uint8_t *src = raw + i * E820_ENTRY_SIZE;
        uint8_t *dst = b + off + 16 + i * 24;
        for (int j = 0; j < 16; j++) {
            dst[j] = src[j];
        }
        for (int j = 0; j < 4; j++) {
            dst[16 + j] = src[16 + j];
        }
        dst[20] = 0;
        dst[21] = 0;
        dst[22] = 0;
        dst[23] = 0;
    }
    return off + align8(tag_size);
}

unsigned int append_framebuffer_tag(unsigned int buf, unsigned int off, unsigned long long fb_addr,
                                     unsigned int pitch, unsigned int width, unsigned int height,
                                     unsigned char bpp, unsigned char red_pos, unsigned char red_size,
                                     unsigned char green_pos, unsigned char green_size,
                                     unsigned char blue_pos, unsigned char blue_size) {
    uint8_t *b = (uint8_t *)(unsigned long long)buf;
    uint32_t tag_size = 38;
    u32_set(b, (int)off, 8);
    u32_set(b, (int)off + 4, tag_size);
    u64_set(b, (int)off + 8, fb_addr);
    u32_set(b, (int)off + 16, pitch);
    u32_set(b, (int)off + 20, width);
    u32_set(b, (int)off + 24, height);
    b[off + 28] = bpp;
    b[off + 29] = 1;
    b[off + 30] = 0;
    b[off + 31] = 0;
    b[off + 32] = red_pos;
    b[off + 33] = red_size;
    b[off + 34] = green_pos;
    b[off + 35] = green_size;
    b[off + 36] = blue_pos;
    b[off + 37] = blue_size;
    return off + align8(tag_size);
}

unsigned int append_rsdp_tag(unsigned int buf, unsigned int off, uintptr_t rsdp_addr) {
    uint8_t *b = (uint8_t *)(unsigned long long)buf;
    const uint8_t *rsdp = (const uint8_t *)rsdp_addr;
    uint8_t revision = rsdp[15];
    uint32_t rsdp_size = 20;
    uint32_t tag_type = 14;
    if (revision >= 2) {
        rsdp_size = u32_get(rsdp, 20);
        if (rsdp_size < 20 || rsdp_size > 64) {
            rsdp_size = 36;
        }
        tag_type = 15;
    }
    uint32_t tag_size = 8 + rsdp_size;
    u32_set(b, (int)off, tag_type);
    u32_set(b, (int)off + 4, tag_size);
    for (uint32_t i = 0; i < rsdp_size; i++) {
        b[off + 8 + i] = rsdp[i];
    }
    return off + align8(tag_size);
}

unsigned int finalize_mbi(unsigned int buf, unsigned int off) {
    uint8_t *b = (uint8_t *)(unsigned long long)buf;
    u32_set(b, (int)off, 0);
    u32_set(b, (int)off + 4, 8);
    off += 8;
    u32_set(b, 0, off);
    u32_set(b, 4, 0);
    return off;
}
