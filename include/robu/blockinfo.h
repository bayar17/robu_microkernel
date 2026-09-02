#ifndef ROBU_BLOCKINFO_H
#define ROBU_BLOCKINFO_H

#include "robu/types.h"

#define BLOCK_INFO_OP_PUBLISH 1
#define BLOCK_INFO_OP_QUERY   2

#define BLOCK_DEVICE_ROOT 1
#define BLOCK_DEVICE_DATA 2
#define BLOCK_DEVICE_MAX  2

#define BLOCK_TRANSPORT_NONE   0
#define BLOCK_TRANSPORT_IDE    1
#define BLOCK_TRANSPORT_VIRTIO 2
#define BLOCK_TRANSPORT_XHCI   3

#define BLOCK_FS_NONE   0
#define BLOCK_FS_EXT2   1
#define BLOCK_FS_FAT16  2
#define BLOCK_FS_FAT32  3
#define BLOCK_FS_DISKFS 4

typedef struct {
    uint32_t in_use;
    uint32_t device;
    uint32_t transport;
    uint32_t filesystem;
    uint32_t sector_size;
    uint32_t reserved;
    uint64_t sectors;
} block_device_info_t;

int kinfo_block_info_publish(uint32_t device, uint32_t transport,
                             uint32_t filesystem, uint32_t sector_size,
                             uint64_t sectors);
int kinfo_block_info_query(uint32_t index, block_device_info_t *out);

#endif
