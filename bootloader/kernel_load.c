typedef unsigned char uint8_t;
typedef unsigned short uint16_t;
typedef unsigned int uint32_t;

#include "ext2_read.h"
#include "mbi_tags.h"

extern void jump_to_kernel(uint32_t entry, uint32_t mbi_ptr) __attribute__((noreturn));
extern uint32_t bios_get_e820(uint32_t dst);

#define ELF_HDR_SCRATCH 0x90000
#define PHDR_SCRATCH 0x91000
#define MBI_BUF 0x92000
#define E820_RAW_BUF 0x93000
#define BOOTSTRAP_LOAD_ADDR 0x6000000
#define BOOTSTRAP_LOAD_LIMIT 0x7000000
#define MAX_PHDRS 16
#define E820_ENTRY_SIZE 20

static uint32_t u32_get(const uint8_t *buf, int off) {
    return (uint32_t)buf[off] | ((uint32_t)buf[off + 1] << 8) |
           ((uint32_t)buf[off + 2] << 16) | ((uint32_t)buf[off + 3] << 24);
}

static uint16_t u16_get(const uint8_t *buf, int off) {
    return (uint16_t)buf[off] | ((uint16_t)buf[off + 1] << 8);
}

static int check_rsdp_sig(uint32_t addr) {
    const uint8_t *p = (const uint8_t *)addr;
    static const uint8_t sig[8] = {'R', 'S', 'D', ' ', 'P', 'T', 'R', ' '};
    for (int i = 0; i < 8; i++) {
        if (p[i] != sig[i]) {
            return 0;
        }
    }
    uint8_t sum = 0;
    for (int i = 0; i < 20; i++) {
        sum = (uint8_t)(sum + p[i]);
    }
    return sum == 0;
}

static int find_rsdp(uint32_t *out_addr) {
    uint16_t ebda_seg = *(const volatile uint16_t *)0x40e;
    uint32_t ebda_addr = (uint32_t)ebda_seg << 4;
    if (ebda_addr != 0) {
        for (uint32_t addr = ebda_addr; addr < ebda_addr + 1024; addr += 16) {
            if (check_rsdp_sig(addr)) {
                *out_addr = addr;
                return 0;
            }
        }
    }
    for (uint32_t addr = 0xe0000; addr < 0x100000; addr += 16) {
        if (check_rsdp_sig(addr)) {
            *out_addr = addr;
            return 0;
        }
    }
    return -1;
}

static int load_elf_segments(unsigned int ino, uint32_t *out_entry) {
    if (ext2_read_range(ino, 0, 64, ELF_HDR_SCRATCH) != 0) {
        return -1;
    }
    uint8_t *ehdr = (uint8_t *)ELF_HDR_SCRATCH;
    if (ehdr[0] != 0x7f || ehdr[1] != 'E' || ehdr[2] != 'L' || ehdr[3] != 'F') {
        return -1;
    }
    if (ehdr[4] != 2) {
        return -1;
    }
    uint32_t e_entry = u32_get(ehdr, 24);
    uint32_t e_phoff = u32_get(ehdr, 32);
    uint16_t e_phentsize = u16_get(ehdr, 54);
    uint16_t e_phnum = u16_get(ehdr, 56);

    if (e_phnum > MAX_PHDRS || e_phentsize < 56) {
        return -1;
    }
    if (ext2_read_range(ino, e_phoff, (uint32_t)e_phentsize * e_phnum, PHDR_SCRATCH) != 0) {
        return -1;
    }

    for (int i = 0; i < e_phnum; i++) {
        uint8_t *ph = (uint8_t *)(PHDR_SCRATCH + i * e_phentsize);
        uint32_t p_type = u32_get(ph, 0);
        if (p_type != 1) {
            continue;
        }
        uint32_t p_offset = u32_get(ph, 8);
        uint32_t p_paddr = u32_get(ph, 24);
        uint32_t p_filesz = u32_get(ph, 32);
        uint32_t p_memsz = u32_get(ph, 40);

        if (p_filesz > 0) {
            if (ext2_read_range(ino, p_offset, p_filesz, p_paddr) != 0) {
                return -1;
            }
        }
        if (p_memsz > p_filesz) {
            uint8_t *bss = (uint8_t *)(p_paddr + p_filesz);
            uint32_t bss_len = p_memsz - p_filesz;
            for (uint32_t j = 0; j < bss_len; j++) {
                bss[j] = 0;
            }
        }
    }

    *out_entry = e_entry;
    return 0;
}

struct bootstrap_module {
    const char *name;
    const char *path;
};

static int load_bootstrap_modules(uint32_t *off) {
    static const struct bootstrap_module modules[] = {
        {"pager", "/boot/bootstrap/pager"},
        {"root_task", "/boot/bootstrap/root_task"},
        {"devfs", "/boot/bootstrap/devfs"},
        {"console_driver", "/boot/bootstrap/console_driver"},
        {"ext2fsroot", "/boot/bootstrap/ext2fsroot"},
        {"procfs", "/boot/bootstrap/procfs"},
        {"sysfs", "/boot/bootstrap/sysfs"},
        {"blockdrv", "/boot/bootstrap/blockdrv"},
        {"diskfs", "/boot/bootstrap/diskfs"},
    };
    uint32_t dst = BOOTSTRAP_LOAD_ADDR;
    for (unsigned int i = 0; i < sizeof(modules) / sizeof(modules[0]); i++) {
        unsigned int ino, size;
        int is_dir;
        if (ext2_resolve_path(modules[i].path, &ino, &size, &is_dir) != 0 || is_dir || size == 0 ||
            dst > BOOTSTRAP_LOAD_LIMIT || size > BOOTSTRAP_LOAD_LIMIT - dst) {
            return -1;
        }
        if (ext2_read_file(ino, size, dst) != 0) {
            return -1;
        }
        *off = append_module_tag(MBI_BUF, *off, dst, dst + size, modules[i].name);
        dst = (dst + size + 4095) & ~4095u;
    }
    return 0;
}

int boot_kernel(void) {
    unsigned int kernel_ino, kernel_size;
    int is_dir;
    if (ext2_resolve_path("/boot/kernel.elf", &kernel_ino, &kernel_size, &is_dir) != 0 || is_dir) {
        return -1;
    }

    uint32_t entry;
    if (load_elf_segments(kernel_ino, &entry) != 0) {
        return -2;
    }

    uint32_t e820_count = bios_get_e820(E820_RAW_BUF);
    uint32_t rsdp_addr;
    int have_rsdp = (find_rsdp(&rsdp_addr) == 0);

    static char cmdline_buf[256];
    const char *cmdline = "root=root_task starter=hello_initsys";
    unsigned int cmdline_ino, cmdline_size;
    if (ext2_resolve_path("/boot/cmdline.txt", &cmdline_ino, &cmdline_size, &is_dir) == 0 &&
        !is_dir && cmdline_size < sizeof(cmdline_buf)) {
        if (ext2_read_file(cmdline_ino, cmdline_size, (unsigned int)cmdline_buf) == 0) {
            cmdline_buf[cmdline_size] = '\0';
            while (cmdline_size > 0 &&
                   (cmdline_buf[cmdline_size - 1] == '\n' || cmdline_buf[cmdline_size - 1] == '\r')) {
                cmdline_buf[--cmdline_size] = '\0';
            }
            cmdline = cmdline_buf;
        }
    }

    uint32_t off = 8;
    off = append_cmdline_tag(MBI_BUF, off, cmdline);
    if (load_bootstrap_modules(&off) != 0) {
        return -3;
    }

    off = append_mmap_tag(MBI_BUF, off, E820_RAW_BUF, e820_count);
    if (have_rsdp) {
        off = append_rsdp_tag(MBI_BUF, off, rsdp_addr);
    }
    finalize_mbi(MBI_BUF, off);

    jump_to_kernel(entry, MBI_BUF);
    return 0;
}
