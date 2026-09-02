#ifndef ROBU_HWDRV_H
#define ROBU_HWDRV_H
#include "robu/types.h"

#define SYS_INFO_CAT_PCI_CFG    37
#define SYS_INFO_CAT_HW_PORT_IO 38
#define SYS_INFO_CAT_DMA_ALLOC  39
#define SYS_INFO_CAT_HW_PORT_IO_BULK_READ  40
#define SYS_INFO_CAT_HW_PORT_IO_BULK_WRITE 41
#define SYS_INFO_CAT_HW_MMIO_MAP 43
#define HW_PORT_IO_BULK_READ_MAX 40
#define HW_PORT_IO_BULK_WRITE_MAX 24

#define DMA_USER_VA_BASE 0x0000000180000000ULL
#define HW_MMIO_USER_VA_BASE 0x00000001A0000000ULL
#define HW_MMIO_MAP_MAX (16ULL * 1024 * 1024)

void ipc_grant_hw_driver(tid_t tid);
void ipc_grant_hw_driver_secondary(tid_t tid);
void ipc_grant_hw_port_io_secondary(tid_t tid);
void ipc_grant_pci_cfg_secondary(tid_t tid);

int64_t arch_pci_cfg_access(uint8_t bus, uint8_t device, uint8_t function,
                             uint8_t offset, int width, int is_write, uint64_t value);
int64_t arch_hw_port_io(uint16_t port, int width, int is_write, uint64_t value);
int64_t arch_hw_port_io_bulk_read(uint16_t port, uint8_t *out, int count);
int64_t arch_hw_port_io_bulk_write(uint16_t port, const uint8_t *in, int count);

#endif
