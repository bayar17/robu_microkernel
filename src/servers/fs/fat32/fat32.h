#ifndef ROBU_FAT32_H
#define ROBU_FAT32_H

#include "robu/types.h"
#include "robu/uipc.h"
#include "robu/ipc.h"
#include "robu/vfs.h"
#include "robu/kinfo.h"
#include "block.h"

#define FAT32FS_MOUNT_PREFIX "/mnt/fat32/"
#define FAT32FS_PREFIX_LEN 11
#define FAT32FS_MAX_HANDLES 8
#define FAT32FS_MAX_CLUSTERS 256
#define FAT32FS_ROOT_INO 1
#define FAT32_EOC 0x0FFFFFF8
#define FAT32_EOC_MARK 0x0FFFFFFF
#define FAT32_BAD_CLUSTER 0x0FFFFFF7

typedef struct {
    uint8_t name[11];
    uint8_t attr;
    uint8_t nt_reserved;
    uint8_t create_time_tenth;
    uint16_t create_time;
    uint16_t create_date;
    uint16_t last_access_date;
    uint16_t first_cluster_high;
    uint16_t write_time;
    uint16_t write_date;
    uint16_t first_cluster_low;
    uint32_t file_size;
} __attribute__((packed)) fat_dirent_t;
_Static_assert(sizeof(fat_dirent_t) == 32, "fat_dirent_t must be exactly 32 bytes");

typedef struct {
    uint16_t bytes_per_sector;
    uint8_t sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t num_fats;
    uint32_t fat_size;
    uint32_t total_sectors;
    uint32_t root_cluster;
    uint32_t fat_start;
    uint32_t data_start;
    uint32_t total_clusters;
} fat32_info_t;
static fat32_info_t g_fat;

static uint32_t g_root_chain[FAT32FS_MAX_CLUSTERS];
static int g_root_num_clusters;

typedef struct {
    int in_use;
    uint32_t clusters[FAT32FS_MAX_CLUSTERS];
    int num_clusters;
    uint32_t file_size;
    uint64_t offset;
    uint32_t dirent_sector;
    uint32_t dirent_offset;
} fat32_handle_t;
static fat32_handle_t handles[FAT32FS_MAX_HANDLES];

#endif
