#ifndef ARCH_X86_64_PCI_H
#define ARCH_X86_64_PCI_H
#include "robu/types.h"
typedef struct {
    uint8_t bus;
    uint8_t device;
    uint8_t function;
} pci_addr_t;
uint32_t pci_cfg_read32(pci_addr_t addr, uint8_t offset);
uint16_t pci_cfg_read16(pci_addr_t addr, uint8_t offset);
uint8_t pci_cfg_read8(pci_addr_t addr, uint8_t offset);
void pci_cfg_write32(pci_addr_t addr, uint8_t offset, uint32_t value);
void pci_cfg_write16(pci_addr_t addr, uint8_t offset, uint16_t value);
void pci_cfg_write8(pci_addr_t addr, uint8_t offset, uint8_t value);

int pci_find_device(uint16_t vendor, uint16_t device, pci_addr_t *out_addr);

void pci_log_devices(void);
#endif
