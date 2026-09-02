static uint32_t cluster_to_sector(uint32_t cluster) {
    return g_fat.data_start + (cluster - 2) * g_fat.sectors_per_cluster;
}

static uint32_t fat_read_entry(uint32_t cluster) {
    static uint8_t fatbuf[512];
    uint32_t fat_offset = cluster * 4;
    uint32_t fat_sector = g_fat.fat_start + fat_offset / g_fat.bytes_per_sector;
    uint32_t entry_offset = fat_offset % g_fat.bytes_per_sector;
    if (blkdev_read(fat_sector, 1, fatbuf) != 0) {
        return 0x0FFFFFFF;
    }
    uint32_t val = (uint32_t)fatbuf[entry_offset] | ((uint32_t)fatbuf[entry_offset + 1] << 8) |
                   ((uint32_t)fatbuf[entry_offset + 2] << 16) | ((uint32_t)fatbuf[entry_offset + 3] << 24);
    return val & 0x0FFFFFFF;
}

static int fat_write_entry(uint32_t cluster, uint32_t value) {
    static uint8_t fatbuf[512];
    uint32_t fat_offset = cluster * 4;
    uint32_t sector_in_fat = fat_offset / g_fat.bytes_per_sector;
    uint32_t entry_offset = fat_offset % g_fat.bytes_per_sector;
    for (uint32_t copy = 0; copy < g_fat.num_fats; copy++) {
        uint32_t fat_sector = g_fat.fat_start + copy * g_fat.fat_size + sector_in_fat;
        if (blkdev_read(fat_sector, 1, fatbuf) != 0) {
            return -1;
        }
        uint32_t old = (uint32_t)fatbuf[entry_offset] | ((uint32_t)fatbuf[entry_offset + 1] << 8) |
                       ((uint32_t)fatbuf[entry_offset + 2] << 16) | ((uint32_t)fatbuf[entry_offset + 3] << 24);
        uint32_t new_val = (old & 0xF0000000u) | (value & 0x0FFFFFFFu);
        fatbuf[entry_offset] = (uint8_t)new_val;
        fatbuf[entry_offset + 1] = (uint8_t)(new_val >> 8);
        fatbuf[entry_offset + 2] = (uint8_t)(new_val >> 16);
        fatbuf[entry_offset + 3] = (uint8_t)(new_val >> 24);
        if (blkdev_write(fat_sector, 1, fatbuf) != 0) {
            return -1;
        }
    }
    return 0;
}

static int walk_chain_into(uint32_t first_cluster, uint32_t *out_clusters, int *out_num) {
    *out_num = 0;
    uint32_t cluster = first_cluster;
    int iters = 0;
    while (cluster >= 2 && cluster < FAT32_EOC && iters < FAT32FS_MAX_CLUSTERS * 4) {
        if (*out_num >= FAT32FS_MAX_CLUSTERS) {
            return -1;
        }
        out_clusters[(*out_num)++] = cluster;
        uint32_t next = fat_read_entry(cluster);
        if (next == 0x00000000 || next == FAT32_BAD_CLUSTER) {
            break;
        }
        cluster = next;
        iters++;
    }
    return 0;
}

static int walk_chain(uint32_t first_cluster, fat32_handle_t *h) {
    return walk_chain_into(first_cluster, h->clusters, &h->num_clusters);
}

static void free_chain(uint32_t first_cluster) {
    uint32_t cluster = first_cluster;
    int iters = 0;
    while (cluster >= 2 && cluster < FAT32_EOC && iters < FAT32FS_MAX_CLUSTERS * 4) {
        uint32_t next = fat_read_entry(cluster);
        fat_write_entry(cluster, 0x00000000);
        if (next == 0x00000000 || next == FAT32_BAD_CLUSTER) {
            break;
        }
        cluster = next;
        iters++;
    }
}

static int alloc_cluster(uint32_t *out_cluster) {
    static uint8_t fatbuf[512];
    uint32_t entries_per_sector = g_fat.bytes_per_sector / 4;
    for (uint32_t s = 0; s < g_fat.fat_size; s++) {
        if (blkdev_read(g_fat.fat_start + s, 1, fatbuf) != 0) {
            return -1;
        }
        for (uint32_t e = 0; e < entries_per_sector; e++) {
            uint32_t cluster = s * entries_per_sector + e;
            if (cluster < 2) {
                continue;
            }
            if (cluster >= g_fat.total_clusters + 2) {
                return -1;
            }
            uint32_t val = ((uint32_t)fatbuf[e * 4] | ((uint32_t)fatbuf[e * 4 + 1] << 8) |
                            ((uint32_t)fatbuf[e * 4 + 2] << 16) | ((uint32_t)fatbuf[e * 4 + 3] << 24)) &
                           0x0FFFFFFFu;
            if (val == 0x00000000) {
                if (fat_write_entry(cluster, FAT32_EOC_MARK) != 0) {
                    return -1;
                }
                *out_cluster = cluster;
                return 0;
            }
        }
    }
    return -1;
}

static int zero_cluster(uint32_t cluster) {
    static uint8_t zerobuf[512];
    for (int i = 0; i < 512; i++) {
        zerobuf[i] = 0;
    }
    uint32_t sector = cluster_to_sector(cluster);
    for (uint32_t s = 0; s < g_fat.sectors_per_cluster; s++) {
        if (blkdev_write(sector + s, 1, zerobuf) != 0) {
            return -1;
        }
    }
    return 0;
}

static void to_83(const char *name, uint8_t out[11]) {
    for (int i = 0; i < 11; i++) {
        out[i] = ' ';
    }
    int i = 0, j = 0;
    while (name[i] && name[i] != '.' && j < 8) {
        char c = name[i];
        if (c >= 'a' && c <= 'z') {
            c = (char)(c - 32);
        }
        out[j++] = (uint8_t)c;
        i++;
    }
    while (name[i] && name[i] != '.') {
        i++;
    }
    if (name[i] == '.') {
        i++;
        int k = 8;
        while (name[i] && k < 11) {
            char c = name[i];
            if (c >= 'a' && c <= 'z') {
                c = (char)(c - 32);
            }
            out[k++] = (uint8_t)c;
            i++;
        }
    }
}

static int find_in_root(const uint8_t name83[11], uint32_t *out_cluster, uint32_t *out_size,
                         uint32_t *out_dirent_sector, uint32_t *out_dirent_offset) {
    static uint8_t dirbuf[512];
    int per_sector = g_fat.bytes_per_sector / 32;
    for (int ci = 0; ci < g_root_num_clusters; ci++) {
        uint32_t cluster_sector = cluster_to_sector(g_root_chain[ci]);
        for (uint32_t s = 0; s < g_fat.sectors_per_cluster; s++) {
            if (blkdev_read(cluster_sector + s, 1, dirbuf) != 0) {
                return -1;
            }
            fat_dirent_t *entries = (fat_dirent_t *)dirbuf;
            for (int e = 0; e < per_sector; e++) {
                fat_dirent_t *d = &entries[e];
                if (d->name[0] == 0x00) {
                    return -1;
                }
                if (d->name[0] == 0xE5) {
                    continue;
                }
                if (d->attr == 0x0F) {
                    continue;
                }
                int match = 1;
                for (int i = 0; i < 11; i++) {
                    if (d->name[i] != name83[i]) {
                        match = 0;
                        break;
                    }
                }
                if (match) {
                    *out_cluster = ((uint32_t)d->first_cluster_high << 16) | d->first_cluster_low;
                    *out_size = d->file_size;
                    *out_dirent_sector = cluster_sector + s;
                    *out_dirent_offset = (uint32_t)e * 32;
                    return 0;
                }
            }
        }
    }
    return -1;
}

static int find_free_root_slot(uint32_t *out_sector, uint32_t *out_offset) {
    static uint8_t dirbuf[512];
    int per_sector = g_fat.bytes_per_sector / 32;
    for (int ci = 0; ci < g_root_num_clusters; ci++) {
        uint32_t cluster_sector = cluster_to_sector(g_root_chain[ci]);
        for (uint32_t s = 0; s < g_fat.sectors_per_cluster; s++) {
            if (blkdev_read(cluster_sector + s, 1, dirbuf) != 0) {
                return -1;
            }
            fat_dirent_t *entries = (fat_dirent_t *)dirbuf;
            for (int e = 0; e < per_sector; e++) {
                fat_dirent_t *d = &entries[e];
                if (d->name[0] == 0x00 || d->name[0] == 0xE5) {
                    *out_sector = cluster_sector + s;
                    *out_offset = (uint32_t)e * 32;
                    return 0;
                }
            }
        }
    }
    if (g_root_num_clusters >= FAT32FS_MAX_CLUSTERS) {
        return -1;
    }
    uint32_t new_cluster;
    if (alloc_cluster(&new_cluster) != 0) {
        return -1;
    }
    if (zero_cluster(new_cluster) != 0) {
        return -1;
    }
    if (g_root_num_clusters > 0) {
        if (fat_write_entry(g_root_chain[g_root_num_clusters - 1], new_cluster) != 0) {
            return -1;
        }
    }
    g_root_chain[g_root_num_clusters++] = new_cluster;
    *out_sector = cluster_to_sector(new_cluster);
    *out_offset = 0;
    return 0;
}

static int64_t fat_now(void) {
    const volatile kinfo_page_t *k = kinfo_user();
    uint64_t hz = k->clock_hz ? k->clock_hz : 1;
    return (int64_t)(kinfo_read_ticks(k) / hz);
}

static int64_t fat_days_from_civil(int year, int month, int day) {
    year -= month <= 2;
    int era = (year >= 0 ? year : year - 399) / 400;
    unsigned yoe = (unsigned)(year - era * 400);
    unsigned doy = (153 * (unsigned)(month + (month > 2 ? -3 : 9)) + 2) / 5 + (unsigned)day - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (int64_t)era * 146097 + (int64_t)doe - 719468;
}

static void fat_civil_from_days(int64_t days, int *year, int *month, int *day) {
    days += 719468;
    int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    unsigned doe = (unsigned)(days - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int y = (int)yoe + (int)era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    *day = (int)(doy - (153 * mp + 2) / 5 + 1);
    *month = (int)(mp + (mp < 10 ? 3 : -9));
    *year = y + (*month <= 2);
}

static void fat_encode_time(int64_t seconds, uint16_t *date_out, uint16_t *time_out) {
    int64_t days = seconds / 86400;
    int64_t day_seconds = seconds % 86400;
    if (day_seconds < 0) {
        days--;
        day_seconds += 86400;
    }
    int year, month, day;
    fat_civil_from_days(days, &year, &month, &day);
    if (year < 1980) {
        year = 1980;
        month = 1;
        day = 1;
        day_seconds = 0;
    } else if (year > 2107) {
        year = 2107;
        month = 12;
        day = 31;
        day_seconds = 86398;
    }
    *date_out = (uint16_t)(((year - 1980) << 9) | (month << 5) | day);
    *time_out = (uint16_t)(((day_seconds / 3600) << 11) |
                           (((day_seconds / 60) % 60) << 5) |
                           ((day_seconds % 60) / 2));
}

static int64_t fat_decode_time(uint16_t date, uint16_t time) {
    int day = date & 0x1f;
    int month = (date >> 5) & 0x0f;
    int year = 1980 + ((date >> 9) & 0x7f);
    if (day == 0 || month == 0 || month > 12) return 0;
    return fat_days_from_civil(year, month, day) * 86400 +
           ((int64_t)((time >> 11) & 0x1f) * 3600) +
           ((int64_t)((time >> 5) & 0x3f) * 60) +
           ((int64_t)(time & 0x1f) * 2);
}

static void fat_set_access_time(fat_dirent_t *d, int64_t atime) {
    uint16_t date, time;
    fat_encode_time(atime, &date, &time);
    d->last_access_date = date;
}

static void fat_set_write_time(fat_dirent_t *d, int64_t mtime) {
    uint16_t date, time;
    fat_encode_time(mtime, &date, &time);
    d->write_date = date;
    d->write_time = time;
}

static int read_dirent(uint32_t sector, uint32_t offset, fat_dirent_t *out) {
    static uint8_t dirbuf[512];
    if (blkdev_read(sector, 1, dirbuf) != 0) return -1;
    *out = *(fat_dirent_t *)(dirbuf + offset);
    return 0;
}

static int write_new_dirent(uint32_t sector, uint32_t offset, const uint8_t name83[11]) {
    static uint8_t dirbuf[512];
    if (blkdev_read(sector, 1, dirbuf) != 0) {
        return -1;
    }
    fat_dirent_t *d = (fat_dirent_t *)(dirbuf + offset);
    for (int i = 0; i < 11; i++) {
        d->name[i] = name83[i];
    }
    d->attr = 0x20;
    d->nt_reserved = 0;
    int64_t now = fat_now();
    uint16_t date, time;
    fat_encode_time(now, &date, &time);
    d->create_time_tenth = 0;
    d->create_time = time;
    d->create_date = date;
    fat_set_access_time(d, now);
    d->first_cluster_high = 0;
    fat_set_write_time(d, now);
    d->first_cluster_low = 0;
    d->file_size = 0;
    return blkdev_write(sector, 1, dirbuf);
}

static int update_dirent_times(fat32_handle_t *hd, int64_t atime, int64_t mtime, uint64_t flags) {
    static uint8_t dirbuf[512];
    if (blkdev_read(hd->dirent_sector, 1, dirbuf) != 0) {
        return -1;
    }
    fat_dirent_t *d = (fat_dirent_t *)(dirbuf + hd->dirent_offset);
    uint32_t first_cluster = hd->num_clusters > 0 ? hd->clusters[0] : 0;
    d->first_cluster_low = (uint16_t)(first_cluster & 0xFFFF);
    d->first_cluster_high = (uint16_t)((first_cluster >> 16) & 0xFFFF);
    d->file_size = hd->file_size;
    if (!(flags & VFS_UTIME_OMIT_ATIME)) fat_set_access_time(d, atime);
    if (!(flags & VFS_UTIME_OMIT_MTIME)) fat_set_write_time(d, mtime);
    return blkdev_write(hd->dirent_sector, 1, dirbuf);
}

static int update_dirent(fat32_handle_t *hd) {
    int64_t now = fat_now();
    return update_dirent_times(hd, now, now, 0);
}

static int fat32_mount(void) {
    if (blkdev_probe() != 0) {
        return -1;
    }
    static uint8_t bpb[512];
    if (blkdev_read(0, 1, bpb) != 0) {
        return -1;
    }
    uint16_t bytes_per_sector = (uint16_t)bpb[11] | ((uint16_t)bpb[12] << 8);
    uint8_t sectors_per_cluster = bpb[13];
    uint16_t reserved_sectors = (uint16_t)bpb[14] | ((uint16_t)bpb[15] << 8);
    uint8_t num_fats = bpb[16];
    uint16_t tot_sec16 = (uint16_t)bpb[19] | ((uint16_t)bpb[20] << 8);
    uint32_t tot_sec32 = (uint32_t)bpb[32] | ((uint32_t)bpb[33] << 8) |
                          ((uint32_t)bpb[34] << 16) | ((uint32_t)bpb[35] << 24);
    uint32_t fat_size32 = (uint32_t)bpb[36] | ((uint32_t)bpb[37] << 8) |
                           ((uint32_t)bpb[38] << 16) | ((uint32_t)bpb[39] << 24);
    uint32_t root_cluster = (uint32_t)bpb[44] | ((uint32_t)bpb[45] << 8) |
                             ((uint32_t)bpb[46] << 16) | ((uint32_t)bpb[47] << 24);

    uint32_t total_sectors = tot_sec16 != 0 ? (uint32_t)tot_sec16 : tot_sec32;

    if (bytes_per_sector != 512 || sectors_per_cluster == 0 || num_fats == 0 ||
        fat_size32 == 0 || total_sectors == 0 || root_cluster < 2) {
        return -1;
    }

    g_fat.bytes_per_sector = bytes_per_sector;
    g_fat.sectors_per_cluster = sectors_per_cluster;
    g_fat.reserved_sectors = reserved_sectors;
    g_fat.num_fats = num_fats;
    g_fat.fat_size = fat_size32;
    g_fat.total_sectors = total_sectors;
    g_fat.root_cluster = root_cluster;
    g_fat.fat_start = reserved_sectors;
    g_fat.data_start = g_fat.fat_start + (uint32_t)num_fats * fat_size32;

    if (total_sectors <= g_fat.data_start) {
        return -1;
    }
    uint32_t data_sectors = total_sectors - g_fat.data_start;
    g_fat.total_clusters = data_sectors / sectors_per_cluster;

    if (g_fat.total_clusters < 65525) {
        return -1;
    }

    if (walk_chain_into(root_cluster, g_root_chain, &g_root_num_clusters) != 0) {
        return -1;
    }
    if (g_root_num_clusters == 0) {
        return -1;
    }
    return 0;
}
