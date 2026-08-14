#include "robu/types.h"
#include "robu/framebuffer.h"
#include "robu/kprintf.h"
#include "portio.h"
#include "pci.h"

#define VBE_DISPI_IOPORT_INDEX 0x1CE
#define VBE_DISPI_IOPORT_DATA  0x1CF

#define VBE_DISPI_INDEX_ID          0
#define VBE_DISPI_INDEX_XRES        1
#define VBE_DISPI_INDEX_YRES        2
#define VBE_DISPI_INDEX_BPP         3
#define VBE_DISPI_INDEX_ENABLE      4
#define VBE_DISPI_INDEX_VIRT_WIDTH  6
#define VBE_DISPI_INDEX_VIRT_HEIGHT 7

#define VBE_DISPI_DISABLED    0x00
#define VBE_DISPI_ENABLED     0x01
#define VBE_DISPI_LFB_ENABLED 0x40

static void vbe_write(uint16_t index, uint16_t value) {
    outw(VBE_DISPI_IOPORT_INDEX, index);
    outw(VBE_DISPI_IOPORT_DATA, value);
}

static uint16_t vbe_read(uint16_t index) {
    outw(VBE_DISPI_IOPORT_INDEX, index);
    return inw(VBE_DISPI_IOPORT_DATA);
}

int vbe_set_mode(uint32_t width, uint32_t height, uint32_t bpp, fb_info_t *out) {
    pci_addr_t addr;
    if (pci_find_device(0x1234, 0x1111, &addr) != 0) {
        return -1;
    }
    uint16_t id = vbe_read(VBE_DISPI_INDEX_ID);
    if (id < 0xB0C0) {
        kprintf("[vbe] unexpected dispi id 0x%x, giving up\n", id);
        return -1;
    }
    uint32_t bar0 = pci_cfg_read32(addr, 0x10);
    if (bar0 & 0x1) {
        kprintf("[vbe] BAR0 is an I/O-space BAR, expected memory-space\n");
        return -1;
    }
    uint64_t fb_phys = bar0 & ~0xFu;
    if (((bar0 >> 1) & 0x3) == 0x2) {
        uint32_t bar1 = pci_cfg_read32(addr, 0x14);
        fb_phys |= ((uint64_t)bar1) << 32;
    }

    vbe_write(VBE_DISPI_INDEX_ENABLE, VBE_DISPI_DISABLED);
    vbe_write(VBE_DISPI_INDEX_XRES, (uint16_t)width);
    vbe_write(VBE_DISPI_INDEX_YRES, (uint16_t)height);
    vbe_write(VBE_DISPI_INDEX_BPP, (uint16_t)bpp);
    vbe_write(VBE_DISPI_INDEX_VIRT_WIDTH, (uint16_t)width);
    vbe_write(VBE_DISPI_INDEX_VIRT_HEIGHT, (uint16_t)height);
    vbe_write(VBE_DISPI_INDEX_ENABLE, VBE_DISPI_ENABLED | VBE_DISPI_LFB_ENABLED);

    out->phys_addr = fb_phys;
    out->pitch = width * (bpp / 8);
    out->width = width;
    out->height = height;
    out->bpp = (uint8_t)bpp;
    out->type = 1;
    out->red_pos = 16;
    out->red_size = 8;
    out->green_pos = 8;
    out->green_size = 8;
    out->blue_pos = 0;
    out->blue_size = 8;
    return 0;
}
