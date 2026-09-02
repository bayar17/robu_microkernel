#ifndef BLKDEV_SLAVE_H
#define BLKDEV_SLAVE_H
#include "robu/types.h"

int blkdev_probe(void);
int blkdev_read(uint64_t sector, uint32_t count, void *buf);
int blkdev_write(uint64_t sector, uint32_t count, const void *buf);
uint64_t blkdev_capacity_sectors(void);

#endif
