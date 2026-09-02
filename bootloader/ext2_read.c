typedef unsigned char uint8_t;
typedef unsigned short uint16_t;
typedef unsigned int uint32_t;
typedef __UINTPTR_TYPE__ uintptr_t;

extern uint32_t bios_read_sectors(uint32_t lba, uint32_t count, uintptr_t dst);

#define EXT2_PARTITION_START_LBA 2048
#define EXT2_ROOT_INODE 2
#define EXT2_S_IFDIR 0x4000
#define EXT2_MAX_GROUPS 8
#define EXT2_MAX_DIR_BLOCKS 12

static uint32_t g_block_size;
static uint32_t g_sectors_per_block;
static uint32_t g_first_data_block;
static uint32_t g_blocks_per_group;
static uint32_t g_inodes_per_group;
static uint32_t g_inode_size;
static uint32_t g_num_groups;
static uint32_t g_gdt_start_byte;
static int g_mounted;

static uint8_t g_sb[1024];
static uint8_t g_blockbuf[4096];

static uint32_t u32_get(const uint8_t *buf, int off) {
    return (uint32_t)buf[off] | ((uint32_t)buf[off + 1] << 8) |
           ((uint32_t)buf[off + 2] << 16) | ((uint32_t)buf[off + 3] << 24);
}

static uint16_t u16_get(const uint8_t *buf, int off) {
    return (uint16_t)buf[off] | ((uint16_t)buf[off + 1] << 8);
}

static int block_read(uint32_t block_num, uint8_t *dst) {
    uint32_t lba = EXT2_PARTITION_START_LBA + block_num * g_sectors_per_block;
    uint32_t got = bios_read_sectors(lba, g_sectors_per_block, (uintptr_t)dst);
    return got == g_sectors_per_block ? 0 : -1;
}

static int gd_read(uint32_t group, uint8_t *out) {
    uint32_t byte_off = g_gdt_start_byte + group * 32;
    uint32_t block_num = byte_off / g_block_size;
    uint32_t off_in_block = byte_off % g_block_size;
    if (block_read(block_num, g_blockbuf) != 0) {
        return -1;
    }
    for (int i = 0; i < 32; i++) {
        out[i] = g_blockbuf[off_in_block + i];
    }
    return 0;
}

static int inode_read(uint32_t ino, uint8_t *out) {
    uint32_t group = (ino - 1) / g_inodes_per_group;
    uint32_t idx = (ino - 1) % g_inodes_per_group;
    uint8_t gd[32];
    if (gd_read(group, gd) != 0) {
        return -1;
    }
    uint32_t inode_table_block = u32_get(gd, 8);
    uint32_t byte_off = inode_table_block * g_block_size + idx * g_inode_size;
    uint32_t block_num = byte_off / g_block_size;
    uint32_t off_in_block = byte_off % g_block_size;
    if (block_read(block_num, g_blockbuf) != 0) {
        return -1;
    }
    for (int i = 0; i < 128; i++) {
        out[i] = g_blockbuf[off_in_block + i];
    }
    return 0;
}

static int find_in_dir(uint32_t dir_ino, const char *name, int name_len,
                        uint32_t *out_ino, uint32_t *out_size, int *out_is_dir) {
    uint8_t inode_buf[128];
    if (inode_read(dir_ino, inode_buf) != 0) {
        return -1;
    }
    uint32_t size = u32_get(inode_buf, 4);
    uint32_t nblocks = (size + g_block_size - 1) / g_block_size;
    if (nblocks > EXT2_MAX_DIR_BLOCKS) {
        nblocks = EXT2_MAX_DIR_BLOCKS;
    }
    for (uint32_t bi = 0; bi < nblocks; bi++) {
        uint32_t blk = u32_get(inode_buf, 40 + bi * 4);
        if (blk == 0) {
            break;
        }
        if (block_read(blk, g_blockbuf) != 0) {
            return -1;
        }
        uint32_t off = 0;
        while (off + 8 <= g_block_size) {
            uint32_t ino = u32_get(g_blockbuf, off);
            uint16_t rec_len = u16_get(g_blockbuf, off + 4);
            uint8_t nl = g_blockbuf[off + 6];
            if (rec_len < 8) {
                break;
            }
            if (ino != 0 && nl == name_len) {
                int match = 1;
                for (int i = 0; i < name_len; i++) {
                    if (g_blockbuf[off + 8 + i] != (uint8_t)name[i]) {
                        match = 0;
                        break;
                    }
                }
                if (match) {
                    uint8_t target_inode[128];
                    if (inode_read(ino, target_inode) != 0) {
                        return -1;
                    }
                    uint16_t mode = u16_get(target_inode, 0);
                    *out_ino = ino;
                    *out_size = u32_get(target_inode, 4);
                    *out_is_dir = (mode & 0xF000) == EXT2_S_IFDIR;
                    return 0;
                }
            }
            off += rec_len;
        }
    }
    return -1;
}

int ext2_mount(void) {
    uint32_t got = bios_read_sectors(EXT2_PARTITION_START_LBA + 2, 2, (uintptr_t)g_sb);
    if (got != 2) {
        return -1;
    }
    if (u16_get(g_sb, 56) != 0xEF53) {
        return -1;
    }
    uint32_t log_block_size = u32_get(g_sb, 24);
    g_block_size = 1024u << log_block_size;
    g_sectors_per_block = g_block_size / 512;
    g_first_data_block = u32_get(g_sb, 20);
    g_blocks_per_group = u32_get(g_sb, 32);
    g_inodes_per_group = u32_get(g_sb, 40);
    uint32_t rev_level = u32_get(g_sb, 76);
    g_inode_size = (rev_level >= 1) ? u16_get(g_sb, 88) : 128;
    uint32_t blocks_count = u32_get(g_sb, 4);
    g_num_groups = (blocks_count + g_blocks_per_group - 1) / g_blocks_per_group;
    if (g_num_groups == 0 || g_num_groups > EXT2_MAX_GROUPS) {
        return -1;
    }
    g_gdt_start_byte = (g_first_data_block + 1) * g_block_size;
    g_mounted = 1;
    return 0;
}

int ext2_resolve_path(const char *path, uint32_t *out_ino, uint32_t *out_size, int *out_is_dir) {
    if (!g_mounted) {
        return -1;
    }
    uint32_t cur_ino = EXT2_ROOT_INODE;
    const char *p = path;
    while (*p == '/') {
        p++;
    }
    for (;;) {
        int seg_len = 0;
        while (p[seg_len] && p[seg_len] != '/') {
            seg_len++;
        }
        if (seg_len == 0) {
            *out_ino = cur_ino;
            *out_size = 0;
            *out_is_dir = 1;
            return 0;
        }
        uint32_t next_ino, next_size;
        int next_is_dir;
        if (find_in_dir(cur_ino, p, seg_len, &next_ino, &next_size, &next_is_dir) != 0) {
            return -1;
        }
        p += seg_len;
        while (*p == '/') {
            p++;
        }
        if (*p == '\0') {
            *out_ino = next_ino;
            *out_size = next_size;
            *out_is_dir = next_is_dir;
            return 0;
        }
        if (!next_is_dir) {
            return -2;
        }
        cur_ino = next_ino;
    }
}

static uint8_t g_indirect_cache[4096];
static uint32_t g_indirect_cache_block = 0xFFFFFFFF;
static uint8_t g_l1_cache[4096];
static uint32_t g_l1_cache_block = 0xFFFFFFFF;

static int cached_block_read(uint32_t block_num, uint8_t *cache_buf, uint32_t *cache_block_num) {
    if (*cache_block_num == block_num) {
        return 0;
    }
    if (block_read(block_num, cache_buf) != 0) {
        return -1;
    }
    *cache_block_num = block_num;
    return 0;
}

static int resolve_data_block(const uint8_t *inode_buf, uint32_t bi, uint32_t *out_blk) {
    uint32_t entries_per_block = g_block_size / 4;
    if (bi < 12) {
        *out_blk = u32_get(inode_buf, 40 + bi * 4);
        return 0;
    }
    uint32_t indirect_idx = bi - 12;
    if (indirect_idx < entries_per_block) {
        uint32_t indirect_block = u32_get(inode_buf, 88);
        if (indirect_block == 0) {
            return -1;
        }
        if (cached_block_read(indirect_block, g_indirect_cache, &g_indirect_cache_block) != 0) {
            return -1;
        }
        *out_blk = u32_get(g_indirect_cache, indirect_idx * 4);
        return 0;
    }
    uint32_t dindirect_idx = indirect_idx - entries_per_block;
    uint32_t dindirect_block = u32_get(inode_buf, 92);
    if (dindirect_block == 0) {
        return -1;
    }
    if (cached_block_read(dindirect_block, g_indirect_cache, &g_indirect_cache_block) != 0) {
        return -1;
    }
    uint32_t l1_idx = dindirect_idx / entries_per_block;
    uint32_t l2_idx = dindirect_idx % entries_per_block;
    uint32_t l1_block = u32_get(g_indirect_cache, l1_idx * 4);
    if (l1_block == 0) {
        return -1;
    }
    if (cached_block_read(l1_block, g_l1_cache, &g_l1_cache_block) != 0) {
        return -1;
    }
    *out_blk = u32_get(g_l1_cache, l2_idx * 4);
    return 0;
}

int ext2_read_file(uint32_t ino, uint32_t size, uintptr_t dst) {
    if (!g_mounted) {
        return -1;
    }
    uint8_t inode_buf[128];
    if (inode_read(ino, inode_buf) != 0) {
        return -1;
    }
    uint32_t nblocks = (size + g_block_size - 1) / g_block_size;
    uint32_t remaining = size;
    for (uint32_t bi = 0; bi < nblocks; bi++) {
        uint32_t blk;
        if (resolve_data_block(inode_buf, bi, &blk) != 0) {
            return -1;
        }
        uint32_t chunk = remaining < g_block_size ? remaining : g_block_size;
        uint8_t *out = (uint8_t *)(dst + (uintptr_t)bi * g_block_size);
        if (blk == 0) {
            for (uint32_t i = 0; i < chunk; i++) {
                out[i] = 0;
            }
            remaining -= chunk;
            continue;
        }
        if (block_read(blk, g_blockbuf) != 0) {
            return -1;
        }
        for (uint32_t i = 0; i < chunk; i++) {
            out[i] = g_blockbuf[i];
        }
        remaining -= chunk;
    }
    return 0;
}

int ext2_read_range(uint32_t ino, uint32_t file_off, uint32_t length, uintptr_t dst) {
    if (!g_mounted) {
        return -1;
    }
    if (length == 0) {
        return 0;
    }
    uint8_t inode_buf[128];
    if (inode_read(ino, inode_buf) != 0) {
        return -1;
    }
    uint32_t end = file_off + length;
    uint32_t bi = file_off / g_block_size;
    uint32_t written = 0;
    while (file_off + written < end) {
        uint32_t block_start = bi * g_block_size;
        uint32_t copy_start_in_block = (file_off + written) - block_start;
        uint32_t avail_in_block = g_block_size - copy_start_in_block;
        uint32_t remaining_in_range = end - (file_off + written);
        uint32_t copy_len = avail_in_block < remaining_in_range ? avail_in_block : remaining_in_range;

        uint32_t blk;
        if (resolve_data_block(inode_buf, bi, &blk) != 0) {
            return -1;
        }
        uint8_t *out = (uint8_t *)(dst + written);
        if (blk == 0) {
            for (uint32_t i = 0; i < copy_len; i++) {
                out[i] = 0;
            }
        } else {
            if (block_read(blk, g_blockbuf) != 0) {
                return -1;
            }
            for (uint32_t i = 0; i < copy_len; i++) {
                out[i] = g_blockbuf[copy_start_in_block + i];
            }
        }
        written += copy_len;
        bi++;
    }
    return 0;
}
