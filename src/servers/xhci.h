#ifndef XHCI_H
#define XHCI_H
#include "robu/types.h"

int xhci_probe(void);
int xhci_read(uint64_t sector, uint32_t count, void *buf);
int xhci_write(uint64_t sector, uint32_t count, const void *buf);
int xhci_flush(void);
uint64_t xhci_capacity_sectors(void);
int xhci_last_failure(void);

#endif
