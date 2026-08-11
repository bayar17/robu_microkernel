#include "pci.h"
#include "portio.h"
#include "robu/kprintf.h"
#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA    0xCFC
#define PCI_VENDOR_ID   0x00
#define PCI_DEVICE_ID   0x02
#define PCI_HEADER_TYPE 0x0E
static uint32_t cfg_address(pci_addr_t addr, uint8_t offset) {
    return (1u << 31) | ((uint32_t)addr.bus << 16) | ((uint32_t)addr.device << 11) |
           ((uint32_t)addr.function << 8) | (uint32_t)(offset & 0xFC);
}
uint32_t pci_cfg_read32(pci_addr_t addr, uint8_t offset) {
    outl(PCI_CONFIG_ADDRESS, cfg_address(addr, offset));
    return inl(PCI_CONFIG_DATA);
}
uint16_t pci_cfg_read16(pci_addr_t addr, uint8_t offset) {
    uint32_t v = pci_cfg_read32(addr, offset);
    return (uint16_t)(v >> ((offset & 2) * 8));
}
uint8_t pci_cfg_read8(pci_addr_t addr, uint8_t offset) {
    uint32_t v = pci_cfg_read32(addr, offset);
    return (uint8_t)(v >> ((offset & 3) * 8));
}
void pci_cfg_write32(pci_addr_t addr, uint8_t offset, uint32_t value) {
    outl(PCI_CONFIG_ADDRESS, cfg_address(addr, offset));
    outl(PCI_CONFIG_DATA, value);
}
void pci_cfg_write16(pci_addr_t addr, uint8_t offset, uint16_t value) {
    uint32_t old = pci_cfg_read32(addr, offset);
    uint32_t shift = (offset & 2) * 8;
    uint32_t mask = 0xFFFFu << shift;
    pci_cfg_write32(addr, offset, (old & ~mask) | ((uint32_t)value << shift));
}
void pci_cfg_write8(pci_addr_t addr, uint8_t offset, uint8_t value) {
    uint32_t old = pci_cfg_read32(addr, offset);
    uint32_t shift = (offset & 3) * 8;
    uint32_t mask = 0xFFu << shift;
    pci_cfg_write32(addr, offset, (old & ~mask) | ((uint32_t)value << shift));
}

static int walk_devices(int (*cb)(pci_addr_t addr, uint16_t vendor, uint16_t device, void *ctx),
                         void *ctx) {
    for (int bus = 0; bus < 256; bus++) {
        for (int dev = 0; dev < 32; dev++) {
            pci_addr_t a0 = { (uint8_t)bus, (uint8_t)dev, 0 };
            uint16_t vid0 = pci_cfg_read16(a0, PCI_VENDOR_ID);
            if (vid0 == 0xFFFF) {
                continue;
            }
            uint8_t header_type = pci_cfg_read8(a0, PCI_HEADER_TYPE);
            int nfuncs = (header_type & 0x80) ? 8 : 1;
            for (int fn = 0; fn < nfuncs; fn++) {
                pci_addr_t a = { (uint8_t)bus, (uint8_t)dev, (uint8_t)fn };
                uint16_t vid = pci_cfg_read16(a, PCI_VENDOR_ID);
                if (vid == 0xFFFF) {
                    continue;
                }
                uint16_t did = pci_cfg_read16(a, PCI_DEVICE_ID);
                if (cb(a, vid, did, ctx)) {
                    return 1;
                }
            }
        }
    }
    return 0;
}
typedef struct {
    uint16_t vendor;
    uint16_t device;
    pci_addr_t *out_addr;
} find_ctx_t;
static int find_cb(pci_addr_t addr, uint16_t vendor, uint16_t device, void *ctx_) {
    find_ctx_t *ctx = (find_ctx_t *)ctx_;
    if (vendor == ctx->vendor && device == ctx->device) {
        *ctx->out_addr = addr;
        return 1;
    }
    return 0;
}
int pci_find_device(uint16_t vendor, uint16_t device, pci_addr_t *out_addr) {
    find_ctx_t ctx = { vendor, device, out_addr };
    return walk_devices(find_cb, &ctx) ? 0 : -1;
}
static int log_cb(pci_addr_t addr, uint16_t vendor, uint16_t device, void *ctx) {
    (void)ctx;
    kprintf("[pci] bus=%x dev=%x fn=%x vendor=%x device=%x\n",
            addr.bus, addr.device, addr.function, vendor, device);
    return 0;
}
void pci_log_devices(void) {
    walk_devices(log_cb, NULL);
}
