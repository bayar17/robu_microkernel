#ifndef ROBU_MBI_TAGS_H
#define ROBU_MBI_TAGS_H

unsigned int align8(unsigned int x);
unsigned int append_cmdline_tag(unsigned int buf, unsigned int off, const char *cmdline);
unsigned int append_module_tag(unsigned int buf, unsigned int off, unsigned int mod_start,
                                unsigned int mod_end, const char *mod_name);
unsigned int append_mmap_tag(unsigned int buf, unsigned int off, __UINTPTR_TYPE__ e820_raw,
                              unsigned int e820_count);
unsigned int append_framebuffer_tag(unsigned int buf, unsigned int off, unsigned long long fb_addr,
                                     unsigned int pitch, unsigned int width, unsigned int height,
                                     unsigned char bpp, unsigned char red_pos, unsigned char red_size,
                                     unsigned char green_pos, unsigned char green_size,
                                     unsigned char blue_pos, unsigned char blue_size);
unsigned int append_rsdp_tag(unsigned int buf, unsigned int off, __UINTPTR_TYPE__ rsdp_addr);
unsigned int finalize_mbi(unsigned int buf, unsigned int off);

#endif
