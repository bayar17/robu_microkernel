#ifndef ROBU_EXT2_BLOCK_H
#define ROBU_EXT2_BLOCK_H
#include "robu/types.h"

int blkdev_probe(void);
int blkdev_read(uint64_t sector, uint32_t count, void *buf);
int blkdev_write(uint64_t sector, uint32_t count, const void *buf);
int blkdev_raw_read(uint64_t sector, uint32_t count, void *buf);
int blkdev_raw_write(uint64_t sector, uint32_t count, const void *buf);
uint64_t blkdev_capacity_sectors(void);
uint32_t blkdev_transport(void);

#endif
