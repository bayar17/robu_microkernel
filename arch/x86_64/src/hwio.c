#include "robu/hwdrv.h"
#include "portio.h"
#include "pci.h"

int64_t arch_pci_cfg_access(uint8_t bus, uint8_t device, uint8_t function,
                             uint8_t offset, int width, int is_write, uint64_t value) {
    pci_addr_t addr = { bus, device, function };
    if (is_write) {
        switch (width) {
        case 1: pci_cfg_write8(addr, offset, (uint8_t)value); return 0;
        case 2: pci_cfg_write16(addr, offset, (uint16_t)value); return 0;
        case 4: pci_cfg_write32(addr, offset, (uint32_t)value); return 0;
        default: return -1;
        }
    }
    switch (width) {
    case 1: return (int64_t)pci_cfg_read8(addr, offset);
    case 2: return (int64_t)pci_cfg_read16(addr, offset);
    case 4: return (int64_t)pci_cfg_read32(addr, offset);
    default: return -1;
    }
}

static int hw_port_allowed(uint16_t port) {
    return (port == 0xCF8 || port == 0xCFC) ||
           (port >= 0x1F0 && port <= 0x1F7) || port == 0x3F6 ||
           (port >= 0x170 && port <= 0x177) || port == 0x376 ||
           (port >= 0xC000);
}

int64_t arch_hw_port_io(uint16_t port, int width, int is_write, uint64_t value) {
    if (!hw_port_allowed(port)) {
        return -1;
    }
    if (is_write) {
        switch (width) {
        case 1: outb(port, (uint8_t)value); return 0;
        case 2: outw(port, (uint16_t)value); return 0;
        case 4: outl(port, (uint32_t)value); return 0;
        default: return -1;
        }
    }
    switch (width) {
    case 1: return (int64_t)inb(port);
    case 2: return (int64_t)inw(port);
    case 4: return (int64_t)inl(port);
    default: return -1;
    }
}

int64_t arch_hw_port_io_bulk_read(uint16_t port, uint8_t *out, int count) {
    if (!hw_port_allowed(port) || count <= 0 || (count & 1)) {
        return -1;
    }
    for (int i = 0; i < count; i += 2) {
        uint16_t w = inw(port);
        out[i] = (uint8_t)w;
        out[i + 1] = (uint8_t)(w >> 8);
    }
    return count;
}

int64_t arch_hw_port_io_bulk_write(uint16_t port, const uint8_t *in, int count) {
    if (!hw_port_allowed(port) || count <= 0 || (count & 1)) {
        return -1;
    }
    for (int i = 0; i < count; i += 2) {
        uint16_t w = (uint16_t)in[i] | ((uint16_t)in[i + 1] << 8);
        outw(port, w);
    }
    return count;
}
