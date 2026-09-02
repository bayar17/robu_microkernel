typedef unsigned char uint8_t;
typedef unsigned short uint16_t;
typedef unsigned int uint32_t;

extern uint32_t bios_read_sectors(uint32_t lba, uint32_t count, uint32_t dst);

#define FAT32_PARTITION_START_LBA 2048
#define FAT32_EOC 0x0FFFFFF8
#define FAT32_MAX_ROOT_CLUSTERS 64
#define FAT32_MAX_FILE_CLUSTERS 4096

static uint32_t g_bytes_per_sector;
static uint32_t g_sectors_per_cluster;
static uint32_t g_reserved_sectors;
static uint32_t g_num_fats;
static uint32_t g_fat_size;
static uint32_t g_total_sectors;
static uint32_t g_root_cluster;
static uint32_t g_fat_start;
static uint32_t g_data_start;
static uint32_t g_total_clusters;

static uint8_t g_bpb[512];
static uint8_t g_secbuf[512];

static uint16_t u16_get(const uint8_t *buf, int off) {
    return (uint16_t)buf[off] | ((uint16_t)buf[off + 1] << 8);
}

static uint32_t u32_get(const uint8_t *buf, int off) {
    return (uint32_t)buf[off] | ((uint32_t)buf[off + 1] << 8) |
           ((uint32_t)buf[off + 2] << 16) | ((uint32_t)buf[off + 3] << 24);
}

static int sector_read(uint32_t sector, uint8_t *dst) {
    uint32_t lba = FAT32_PARTITION_START_LBA + sector;
    return bios_read_sectors(lba, 1, (uint32_t)dst) == 1 ? 0 : -1;
}

static uint32_t cluster_to_sector(uint32_t cluster) {
    return g_data_start + (cluster - 2) * g_sectors_per_cluster;
}

static int fat_entry_read(uint32_t cluster, uint32_t *out_val) {
    uint32_t fat_offset = cluster * 4;
    uint32_t fat_sector = g_fat_start + fat_offset / g_bytes_per_sector;
    uint32_t entry_offset = fat_offset % g_bytes_per_sector;
    if (sector_read(fat_sector, g_secbuf) != 0) {
        return -1;
    }
    *out_val = u32_get(g_secbuf, (int)entry_offset) & 0x0FFFFFFF;
    return 0;
}

int fat32_mount(void) {
    if (bios_read_sectors(FAT32_PARTITION_START_LBA, 1, (uint32_t)g_bpb) != 1) {
        return -1;
    }
    uint16_t bytes_per_sector = u16_get(g_bpb, 11);
    uint8_t sectors_per_cluster = g_bpb[13];
    uint16_t reserved_sectors = u16_get(g_bpb, 14);
    uint8_t num_fats = g_bpb[16];
    uint16_t tot_sec16 = u16_get(g_bpb, 19);
    uint32_t tot_sec32 = u32_get(g_bpb, 32);
    uint32_t fat_size32 = u32_get(g_bpb, 36);
    uint32_t root_cluster = u32_get(g_bpb, 44);

    uint32_t total_sectors = tot_sec16 != 0 ? (uint32_t)tot_sec16 : tot_sec32;

    if (bytes_per_sector != 512 || sectors_per_cluster == 0 || num_fats == 0 ||
        fat_size32 == 0 || total_sectors == 0 || root_cluster < 2) {
        return -1;
    }

    g_bytes_per_sector = bytes_per_sector;
    g_sectors_per_cluster = sectors_per_cluster;
    g_reserved_sectors = reserved_sectors;
    g_num_fats = num_fats;
    g_fat_size = fat_size32;
    g_total_sectors = total_sectors;
    g_root_cluster = root_cluster;
    g_fat_start = reserved_sectors;
    g_data_start = g_fat_start + g_num_fats * g_fat_size;

    if (g_total_sectors <= g_data_start) {
        return -1;
    }
    uint32_t data_sectors = g_total_sectors - g_data_start;
    g_total_clusters = data_sectors / g_sectors_per_cluster;
    if (g_total_clusters < 65525) {
        return -1;
    }
    return 0;
}

int fat32_resolve_root_file(const char *name83, uint32_t *out_cluster, uint32_t *out_size) {
    uint32_t cluster = g_root_cluster;
    uint32_t iters = 0;
    while (cluster >= 2 && cluster < FAT32_EOC && iters < FAT32_MAX_ROOT_CLUSTERS) {
        uint32_t base_sector = cluster_to_sector(cluster);
        for (uint32_t s = 0; s < g_sectors_per_cluster; s++) {
            if (sector_read(base_sector + s, g_secbuf) != 0) {
                return -1;
            }
            int per_sector = 512 / 32;
            for (int i = 0; i < per_sector; i++) {
                const uint8_t *d = g_secbuf + i * 32;
                if (d[0] == 0x00) {
                    return -1;
                }
                if (d[0] == 0xE5) {
                    continue;
                }
                if (d[11] == 0x0F) {
                    continue;
                }
                int match = 1;
                for (int j = 0; j < 11; j++) {
                    if (d[j] != (uint8_t)name83[j]) {
                        match = 0;
                        break;
                    }
                }
                if (match) {
                    uint32_t first_cluster_high = u16_get(d, 20);
                    uint32_t first_cluster_low = u16_get(d, 26);
                    *out_cluster = (first_cluster_high << 16) | first_cluster_low;
                    *out_size = u32_get(d, 28);
                    return 0;
                }
            }
        }
        uint32_t next;
        if (fat_entry_read(cluster, &next) != 0) {
            return -1;
        }
        cluster = next;
        iters++;
    }
    return -1;
}

int fat32_read_file(uint32_t cluster, uint32_t size, uint32_t dst) {
    uint32_t remaining = size;
    uint32_t iters = 0;
    while (cluster >= 2 && cluster < FAT32_EOC && remaining > 0 && iters < FAT32_MAX_FILE_CLUSTERS) {
        uint32_t base_sector = cluster_to_sector(cluster);
        for (uint32_t s = 0; s < g_sectors_per_cluster && remaining > 0; s++) {
            if (sector_read(base_sector + s, g_secbuf) != 0) {
                return -1;
            }
            uint32_t chunk = remaining < 512 ? remaining : 512;
            uint8_t *out = (uint8_t *)dst;
            for (uint32_t i = 0; i < chunk; i++) {
                out[i] = g_secbuf[i];
            }
            dst += chunk;
            remaining -= chunk;
        }
        if (remaining == 0) {
            break;
        }
        uint32_t next;
        if (fat_entry_read(cluster, &next) != 0) {
            return -1;
        }
        cluster = next;
        iters++;
    }
    return remaining == 0 ? 0 : -1;
}
