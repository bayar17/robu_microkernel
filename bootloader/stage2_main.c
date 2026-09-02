typedef unsigned char uint8_t;
typedef unsigned int uint32_t;

extern uint32_t bios_read_sectors(uint32_t lba, uint32_t count, uint32_t dst);

#include "ext2_read.h"
#include "fat32_read.h"

extern int boot_kernel(void);

#define SENTINEL_LBA 300
#define TEST_READ_DST 0x100000
#define EXT2_PARTITION_START_LBA 2048
#define EXT2_TEST_READ_DST 0x110000

#define COM1_PORT 0x3f8

static unsigned char inb(unsigned short port) {
    unsigned char v;
    __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static void outb(unsigned short port, unsigned char v) {
    __asm__ volatile("outb %0, %1" : : "a"(v), "Nd"(port));
}

static void serial_putc(char c) {
    int spin = 100000;
    while (spin-- > 0 && !(inb(COM1_PORT + 5) & 0x20)) {
    }
    outb(COM1_PORT, (unsigned char)c);
}

static void serial_puts(const char *s) {
    while (*s) {
        serial_putc(*s++);
    }
}

static void serial_put_hex(uint32_t v) {
    static const char hexd[] = "0123456789abcdef";
    for (int shift = 28; shift >= 0; shift -= 4) {
        serial_putc(hexd[(v >> shift) & 0xf]);
    }
}

static volatile unsigned short *const VGA = (volatile unsigned short *)0xb8000;
static int vga_row;
static int vga_col;

static void vga_clear(void) {
    for (int i = 0; i < 80 * 25; i++) {
        VGA[i] = 0x0f20;
    }
    vga_row = 0;
    vga_col = 0;
}

static void vga_putc(char c) {
    if (c == '\n') {
        vga_row++;
        vga_col = 0;
        return;
    }
    VGA[vga_row * 80 + vga_col] = (unsigned short)(0x0f00 | (unsigned char)c);
    vga_col++;
}

static void vga_puts(const char *s) {
    while (*s) {
        vga_putc(*s++);
    }
}

static const char hexdigits[] = "0123456789abcdef";

static void vga_put_hex_byte(unsigned char b) {
    vga_putc(hexdigits[(b >> 4) & 0xf]);
    vga_putc(hexdigits[b & 0xf]);
}

void stage2_main(void) {
    vga_clear();
    vga_puts("Robu stage2: reading sentinel sector via thunk\n");

    uint32_t got = bios_read_sectors(SENTINEL_LBA, 1, TEST_READ_DST);
    if (got == 0) {
        vga_puts("thunk read failed\n");
    } else {
        vga_puts("first 16 bytes: ");
        volatile unsigned char *buf = (volatile unsigned char *)TEST_READ_DST;
        for (int i = 0; i < 16; i++) {
            vga_put_hex_byte(buf[i]);
            vga_putc(' ');
        }
    }
    vga_putc('\n');

    vga_puts("ext2 partition sector 0: ");
    uint32_t got2 = bios_read_sectors(EXT2_PARTITION_START_LBA, 1, EXT2_TEST_READ_DST);
    if (got2 == 0) {
        vga_puts("thunk read failed\n");
    } else {
        volatile unsigned char *buf2 = (volatile unsigned char *)EXT2_TEST_READ_DST;
        for (int i = 0; i < 16; i++) {
            vga_put_hex_byte(buf2[i]);
            vga_putc(' ');
        }
    }
    vga_putc('\n');

    vga_puts("ext2 mount: ");
    if (ext2_mount() != 0) {
        vga_puts("FAILED\n");
    } else {
        vga_puts("ok\n");
        vga_puts("resolve /boot/test.txt: ");
        unsigned int ino, size;
        int is_dir;
        if (ext2_resolve_path("/boot/test.txt", &ino, &size, &is_dir) != 0) {
            vga_puts("FAILED\n");
        } else if (is_dir) {
            vga_puts("is a directory\n");
        } else {
            vga_puts("ok, reading: ");
            unsigned int dst = 0x120000;
            if (ext2_read_file(ino, size, dst) != 0) {
                vga_puts("read FAILED\n");
            } else {
                volatile char *content = (volatile char *)dst;
                for (unsigned int i = 0; i < size; i++) {
                    vga_putc(content[i]);
                }
            }
        }
    }

    serial_puts("fat32 mount: ");
    if (fat32_mount() != 0) {
        serial_puts("FAILED\n");
    } else {
        serial_puts("ok\n");
        serial_puts("resolve TESTFILE.TXT: ");
        unsigned int fcluster, fsize;
        unsigned char name83[11] = {
            'T', 'E', 'S', 'T', 'F', 'I', 'L', 'E', 'T', 'X', 'T'
        };
        if (fat32_resolve_root_file((const char *)name83, &fcluster, &fsize) != 0) {
            serial_puts("FAILED\n");
        } else {
            serial_puts("ok cluster=0x");
            serial_put_hex(fcluster);
            serial_puts(" size=0x");
            serial_put_hex(fsize);
            serial_puts("\n");
            if (fsize < 512) {
                unsigned int dst = 0x130000;
                serial_puts("reading: ");
                if (fat32_read_file(fcluster, fsize, dst) != 0) {
                    serial_puts("read FAILED\n");
                } else {
                    volatile char *content = (volatile char *)dst;
                    for (unsigned int i = 0; i < fsize; i++) {
                        serial_putc(content[i]);
                    }
                }
            } else {
                serial_puts("size looks wrong, skipping read\n");
            }
        }
    }

    vga_puts("booting kernel...\n");
    int rc = boot_kernel();
    vga_puts("boot_kernel returned (should never happen): ");
    vga_put_hex_byte((unsigned char)rc);

    for (;;) {
        __asm__ volatile("cli; hlt");
    }
}
