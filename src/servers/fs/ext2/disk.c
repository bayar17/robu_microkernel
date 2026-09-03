#include "ext2.h"

static int journal_metadata_read(uint64_t sector, uint32_t count, void *out);
static int journal_metadata_write(uint64_t sector, uint32_t count, const void *in);
static int journal_mount(void);

static uint32_t u32_get(const uint8_t *buf, int off) {
    return (uint32_t)buf[off] | ((uint32_t)buf[off + 1] << 8) |
           ((uint32_t)buf[off + 2] << 16) | ((uint32_t)buf[off + 3] << 24);
}
static void u32_set(uint8_t *buf, int off, uint32_t v) {
    buf[off] = (uint8_t)v;
    buf[off + 1] = (uint8_t)(v >> 8);
    buf[off + 2] = (uint8_t)(v >> 16);
    buf[off + 3] = (uint8_t)(v >> 24);
}
static uint16_t u16_get(const uint8_t *buf, int off) {
    return (uint16_t)buf[off] | ((uint16_t)buf[off + 1] << 8);
}
static void u16_set(uint8_t *buf, int off, uint16_t v) {
    buf[off] = (uint8_t)v;
    buf[off + 1] = (uint8_t)(v >> 8);
}

static uint8_t g_sb[1024];
static uint32_t g_block_size;
static uint32_t g_sectors_per_block;
static uint32_t g_blocks_count;
static uint32_t g_inodes_count;
static uint32_t g_blocks_per_group;
static uint32_t g_inodes_per_group;
static uint32_t g_first_data_block;
static uint32_t g_inode_size;
static uint32_t g_num_groups;
static uint64_t g_gdt_start_byte;
static int g_dirent_file_type;

static ext2_handle_t handles[EXT2FS_MAX_HANDLES];

static int sb_write(void) {
    return journal_metadata_write(2, 2, g_sb);
}

static int gd_read(uint32_t group, uint8_t out[32]) {
    static uint8_t buf[512];
    uint64_t byte_off = g_gdt_start_byte + (uint64_t)group * 32;
    uint64_t sector = byte_off / 512;
    uint32_t off = (uint32_t)(byte_off % 512);
    if (journal_metadata_read(sector, 1, buf) != 0) {
        return -1;
    }
    for (int i = 0; i < 32; i++) {
        out[i] = buf[off + i];
    }
    return 0;
}

static int gd_write(uint32_t group, const uint8_t in[32]) {
    static uint8_t buf[512];
    uint64_t byte_off = g_gdt_start_byte + (uint64_t)group * 32;
    uint64_t sector = byte_off / 512;
    uint32_t off = (uint32_t)(byte_off % 512);
    if (journal_metadata_read(sector, 1, buf) != 0) {
        return -1;
    }
    for (int i = 0; i < 32; i++) {
        buf[off + i] = in[i];
    }
    return journal_metadata_write(sector, 1, buf);
}

static int block_read(uint32_t block_num, void *buf) {
    return blkdev_read((uint64_t)block_num * g_sectors_per_block, g_sectors_per_block, buf);
}
static int block_write(uint32_t block_num, const void *buf) {
    return blkdev_write((uint64_t)block_num * g_sectors_per_block, g_sectors_per_block, buf);
}

static int metadata_block_read(uint32_t block_num, void *buf) {
    return journal_metadata_read((uint64_t)block_num * g_sectors_per_block,
                                 g_sectors_per_block, buf);
}

static int metadata_block_write(uint32_t block_num, const void *buf) {
    return journal_metadata_write((uint64_t)block_num * g_sectors_per_block,
                                  g_sectors_per_block, buf);
}

static int inode_io(uint32_t ino, uint8_t io[128], int is_write) {
    uint32_t group = (ino - 1) / g_inodes_per_group;
    uint32_t idx = (ino - 1) % g_inodes_per_group;
    uint8_t gd[32];
    if (gd_read(group, gd) != 0) {
        return -1;
    }
    uint32_t inode_table_block = u32_get(gd, 8);
    uint64_t byte_off = (uint64_t)inode_table_block * g_block_size + (uint64_t)idx * g_inode_size;
    uint64_t sector = byte_off / 512;
    uint32_t sec_off = (uint32_t)(byte_off % 512);
    uint32_t nsectors = (sec_off + 128 + 511) / 512;
    static uint8_t buf[1536];
    if (journal_metadata_read(sector, nsectors, buf) != 0) {
        return -1;
    }
    if (is_write) {
        for (int i = 0; i < 128; i++) {
            buf[sec_off + i] = io[i];
        }
        return journal_metadata_write(sector, nsectors, buf);
    }
    for (int i = 0; i < 128; i++) {
        io[i] = buf[sec_off + i];
    }
    return 0;
}

static uint32_t g_alloc_cursor_group;
static uint32_t g_alloc_cursor_bit;

static int alloc_block(uint32_t *out_block) {
    static uint8_t bitmap[EXT2FS_MAX_BLOCK_SIZE];
    for (uint32_t attempt = 0; attempt < g_num_groups + 1; attempt++) {
        uint32_t g = g_alloc_cursor_group;
        uint8_t gd[32];
        if (gd_read(g, gd) != 0) {
            return -1;
        }
        uint16_t free_in_group = u16_get(gd, 12);
        if (free_in_group == 0) {
            g_alloc_cursor_group = (g_alloc_cursor_group + 1) % g_num_groups;
            g_alloc_cursor_bit = 0;
            continue;
        }
        uint32_t bitmap_block = u32_get(gd, 0);
        if (metadata_block_read(bitmap_block, bitmap) != 0) {
            return -1;
        }
        uint64_t group_start = (uint64_t)g_first_data_block + (uint64_t)g * g_blocks_per_group;
        uint64_t group_end = group_start + g_blocks_per_group;
        if (group_end > g_blocks_count) {
            group_end = g_blocks_count;
        }
        uint32_t blocks_in_group = (uint32_t)(group_end - group_start);
        for (uint32_t bit = g_alloc_cursor_bit; bit < blocks_in_group; bit++) {
            uint32_t byte_idx = bit / 8;
            uint32_t bit_idx = bit % 8;
            if (!(bitmap[byte_idx] & (1 << bit_idx))) {
                bitmap[byte_idx] = (uint8_t)(bitmap[byte_idx] | (1 << bit_idx));
                if (metadata_block_write(bitmap_block, bitmap) != 0) {
                    return -1;
                }
                u16_set(gd, 12, (uint16_t)(free_in_group - 1));
                if (gd_write(g, gd) != 0) {
                    return -1;
                }
                u32_set(g_sb, 12, u32_get(g_sb, 12) - 1);
                if (sb_write() != 0) {
                    return -1;
                }
                g_alloc_cursor_bit = bit + 1;
                *out_block = (uint32_t)group_start + bit;
                return 0;
            }
        }
        g_alloc_cursor_group = (g_alloc_cursor_group + 1) % g_num_groups;
        g_alloc_cursor_bit = 0;
    }
    return -1;
}

static int free_block(uint32_t block_num) {
    if (block_num < g_first_data_block) {
        return -1;
    }
    uint32_t rel = block_num - g_first_data_block;
    uint32_t g = rel / g_blocks_per_group;
    uint32_t bit = rel % g_blocks_per_group;
    uint8_t gd[32];
    if (gd_read(g, gd) != 0) {
        return -1;
    }
    static uint8_t bitmap[EXT2FS_MAX_BLOCK_SIZE];
    uint32_t bitmap_block = u32_get(gd, 0);
    if (metadata_block_read(bitmap_block, bitmap) != 0) {
        return -1;
    }
    uint32_t byte_idx = bit / 8;
    uint32_t bit_idx = bit % 8;
    bitmap[byte_idx] = (uint8_t)(bitmap[byte_idx] & ~(1 << bit_idx));
    if (metadata_block_write(bitmap_block, bitmap) != 0) {
        return -1;
    }
    u16_set(gd, 12, (uint16_t)(u16_get(gd, 12) + 1));
    if (gd_write(g, gd) != 0) {
        return -1;
    }
    u32_set(g_sb, 12, u32_get(g_sb, 12) + 1);
    return sb_write();
}

static uint32_t g_alloc_inode_cursor_group;
static uint32_t g_alloc_inode_cursor_bit;

static int alloc_inode(uint32_t *out_ino) {
    static uint8_t bitmap[EXT2FS_MAX_BLOCK_SIZE];
    for (uint32_t attempt = 0; attempt < g_num_groups + 1; attempt++) {
        uint32_t g = g_alloc_inode_cursor_group;
        uint8_t gd[32];
        if (gd_read(g, gd) != 0) {
            return -1;
        }
        uint16_t free_in_group = u16_get(gd, 14);
        if (free_in_group == 0) {
            g_alloc_inode_cursor_group = (g_alloc_inode_cursor_group + 1) % g_num_groups;
            g_alloc_inode_cursor_bit = 0;
            continue;
        }
        uint32_t bitmap_block = u32_get(gd, 4);
        if (metadata_block_read(bitmap_block, bitmap) != 0) {
            return -1;
        }
        for (uint32_t bit = g_alloc_inode_cursor_bit; bit < g_inodes_per_group; bit++) {
            uint32_t byte_idx = bit / 8;
            uint32_t bit_idx = bit % 8;
            if (!(bitmap[byte_idx] & (1 << bit_idx))) {
                bitmap[byte_idx] = (uint8_t)(bitmap[byte_idx] | (1 << bit_idx));
                if (metadata_block_write(bitmap_block, bitmap) != 0) {
                    return -1;
                }
                u16_set(gd, 14, (uint16_t)(free_in_group - 1));
                if (gd_write(g, gd) != 0) {
                    return -1;
                }
                u32_set(g_sb, 16, u32_get(g_sb, 16) - 1);
                if (sb_write() != 0) {
                    return -1;
                }
                g_alloc_inode_cursor_bit = bit + 1;
                *out_ino = g * g_inodes_per_group + bit + 1;
                return 0;
            }
        }
        g_alloc_inode_cursor_group = (g_alloc_inode_cursor_group + 1) % g_num_groups;
        g_alloc_inode_cursor_bit = 0;
    }
    return -1;
}

static int free_inode(uint32_t ino) {
    if (ino == 0 || ino > g_inodes_count) {
        return -1;
    }
    uint32_t group = (ino - 1) / g_inodes_per_group;
    uint32_t bit = (ino - 1) % g_inodes_per_group;
    if (group >= g_num_groups) {
        return -1;
    }
    uint8_t gd[32];
    if (gd_read(group, gd) != 0) {
        return -1;
    }
    static uint8_t bitmap[EXT2FS_MAX_BLOCK_SIZE];
    uint32_t bitmap_block = u32_get(gd, 4);
    if (metadata_block_read(bitmap_block, bitmap) != 0) {
        return -1;
    }
    uint32_t byte_idx = bit / 8;
    uint32_t bit_idx = bit % 8;
    if (!(bitmap[byte_idx] & (1u << bit_idx))) {
        return -1;
    }
    bitmap[byte_idx] = (uint8_t)(bitmap[byte_idx] & ~(1u << bit_idx));
    if (metadata_block_write(bitmap_block, bitmap) != 0) {
        return -1;
    }
    u16_set(gd, 14, (uint16_t)(u16_get(gd, 14) + 1));
    if (gd_write(group, gd) != 0) {
        return -1;
    }
    u32_set(g_sb, 16, u32_get(g_sb, 16) + 1);
    return sb_write();
}

static int walk_inode_blocks(const uint8_t inode_buf[128], uint32_t need_blocks,
                              uint32_t *out_blocks, uint32_t *out_count) {
    uint32_t n = 0;
    for (int i = 0; i < 12 && n < need_blocks; i++) {
        uint32_t b = u32_get(inode_buf, 40 + i * 4);
        if (b == 0) {
            *out_count = n;
            return 0;
        }
        out_blocks[n++] = b;
    }
    uint32_t entries = g_block_size / 4;
    if (n < need_blocks) {
        uint32_t indirect = u32_get(inode_buf, 40 + 12 * 4);
        if (indirect != 0) {
            static uint8_t indbuf[EXT2FS_MAX_BLOCK_SIZE];
            if (metadata_block_read(indirect, indbuf) != 0) {
                return -1;
            }
            for (uint32_t i = 0; i < entries && n < need_blocks; i++) {
                uint32_t b = u32_get(indbuf, i * 4);
                if (b == 0) {
                    *out_count = n;
                    return 0;
                }
                out_blocks[n++] = b;
            }
        }
    }
    if (n < need_blocks) {
        uint32_t dindirect = u32_get(inode_buf, 40 + 13 * 4);
        if (dindirect != 0) {
            static uint8_t dindbuf[EXT2FS_MAX_BLOCK_SIZE];
            if (metadata_block_read(dindirect, dindbuf) != 0) {
                return -1;
            }
            for (uint32_t di = 0; di < entries && n < need_blocks; di++) {
                uint32_t sind = u32_get(dindbuf, di * 4);
                if (sind == 0) {
                    break;
                }
                static uint8_t sindbuf[EXT2FS_MAX_BLOCK_SIZE];
                if (metadata_block_read(sind, sindbuf) != 0) {
                    return -1;
                }
                for (uint32_t i = 0; i < entries && n < need_blocks; i++) {
                    uint32_t b = u32_get(sindbuf, i * 4);
                    if (b == 0) {
                        *out_count = n;
                        return 0;
                    }
                    out_blocks[n++] = b;
                }
            }
        }
    }
    *out_count = n;
    return 0;
}

static int append_block_to_inode(uint8_t inode_buf[128], uint32_t logical_idx, uint32_t new_block) {
    if (logical_idx < 12) {
        u32_set(inode_buf, 40 + logical_idx * 4, new_block);
        return 0;
    }
    uint32_t entries = g_block_size / 4;
    uint32_t idx = logical_idx - 12;
    if (idx < entries) {
        static uint8_t indbuf[EXT2FS_MAX_BLOCK_SIZE];
        uint32_t indirect = u32_get(inode_buf, 40 + 12 * 4);
        if (indirect == 0) {
            if (alloc_block(&indirect) != 0) {
                return -1;
            }
            for (uint32_t i = 0; i < g_block_size; i++) {
                indbuf[i] = 0;
            }
            if (metadata_block_write(indirect, indbuf) != 0) {
                return -1;
            }
            u32_set(inode_buf, 40 + 12 * 4, indirect);
        } else {
            if (metadata_block_read(indirect, indbuf) != 0) {
                return -1;
            }
        }
        u32_set(indbuf, idx * 4, new_block);
        return metadata_block_write(indirect, indbuf);
    }

    idx -= entries;
    uint32_t di = idx / entries;
    uint32_t si = idx % entries;
    if (di >= entries) {
        return -1;
    }

    static uint8_t dindbuf[EXT2FS_MAX_BLOCK_SIZE];
    uint32_t dindirect = u32_get(inode_buf, 40 + 13 * 4);
    if (dindirect == 0) {
        if (alloc_block(&dindirect) != 0) {
            return -1;
        }
        for (uint32_t i = 0; i < g_block_size; i++) {
            dindbuf[i] = 0;
        }
        if (metadata_block_write(dindirect, dindbuf) != 0) {
            return -1;
        }
        u32_set(inode_buf, 40 + 13 * 4, dindirect);
    } else {
        if (metadata_block_read(dindirect, dindbuf) != 0) {
            return -1;
        }
    }

    uint32_t sind = u32_get(dindbuf, di * 4);
    static uint8_t sindbuf[EXT2FS_MAX_BLOCK_SIZE];
    if (sind == 0) {
        if (alloc_block(&sind) != 0) {
            return -1;
        }
        for (uint32_t i = 0; i < g_block_size; i++) {
            sindbuf[i] = 0;
        }
        if (metadata_block_write(sind, sindbuf) != 0) {
            return -1;
        }
        u32_set(dindbuf, di * 4, sind);
        if (metadata_block_write(dindirect, dindbuf) != 0) {
            return -1;
        }
    } else {
        if (metadata_block_read(sind, sindbuf) != 0) {
            return -1;
        }
    }
    u32_set(sindbuf, si * 4, new_block);
    return metadata_block_write(sind, sindbuf);
}

static uint32_t compute_i_blocks_sectors(const uint8_t inode_buf[128], uint32_t num_blocks) {
    uint32_t entries = g_block_size / 4;
    uint32_t sectors = num_blocks * g_sectors_per_block;
    uint32_t indirect = u32_get(inode_buf, 40 + 12 * 4);
    if (indirect != 0) {
        sectors += g_sectors_per_block;
    }
    uint32_t dindirect = u32_get(inode_buf, 40 + 13 * 4);
    if (dindirect != 0) {
        sectors += g_sectors_per_block;
        uint32_t data_beyond_single = (num_blocks > 12 + entries) ? (num_blocks - 12 - entries) : 0;
        uint32_t sind_blocks_used = (data_beyond_single + entries - 1) / entries;
        sectors += sind_blocks_used * g_sectors_per_block;
    }
    if (u32_get(inode_buf, EXT2_I_FILE_ACL) != 0) {
        sectors += g_sectors_per_block;
    }
    return sectors;
}

static int free_inode_blocks(uint8_t inode_buf[128]) {
    uint32_t size = u32_get(inode_buf, 4);
    uint32_t need_blocks = (size + g_block_size - 1) / g_block_size;
    static uint32_t blocks[EXT2FS_MAX_BLOCKS];
    uint32_t nblocks;
    if (walk_inode_blocks(inode_buf, need_blocks, blocks, &nblocks) != 0) {
        return -1;
    }
    for (uint32_t i = 0; i < nblocks; i++) {
        if (free_block(blocks[i]) != 0) {
            return -1;
        }
    }
    uint32_t indirect = u32_get(inode_buf, 40 + 12 * 4);
    if (indirect != 0 && free_block(indirect) != 0) {
        return -1;
    }
    uint32_t dindirect = u32_get(inode_buf, 40 + 13 * 4);
    if (dindirect != 0) {
        static uint8_t dindbuf[EXT2FS_MAX_BLOCK_SIZE];
        if (metadata_block_read(dindirect, dindbuf) != 0) {
            return -1;
        }
        uint32_t entries = g_block_size / 4;
        for (uint32_t i = 0; i < entries; i++) {
            uint32_t sindirect = u32_get(dindbuf, i * 4);
            if (sindirect != 0 && free_block(sindirect) != 0) {
                return -1;
            }
        }
        if (free_block(dindirect) != 0) {
            return -1;
        }
    }
    return 0;
}


static int ext2_mount(void) {
    int probe_rc = blkdev_probe();
    if (probe_rc != 0) {
        return probe_rc;
    }
    int read_rc = blkdev_read(2, 2, g_sb);
    if (read_rc != 0) {
        return read_rc;
    }
    if (u16_get(g_sb, 56) != 0xEF53) {
        return -1;
    }
    uint32_t feature_compat = u32_get(g_sb, 92);
    uint32_t feature_incompat = u32_get(g_sb, 96);
    uint32_t feature_ro_compat = u32_get(g_sb, 100);
    if (feature_compat & EXT2_FEATURE_COMPAT_HAS_JOURNAL) {
        return -1;
    }
    if (feature_incompat & ~EXT2_FEATURE_INCOMPAT_FILETYPE) {
        return -1;
    }
    if (feature_ro_compat & ~(EXT2_FEATURE_RO_COMPAT_SPARSE_SUPER |
                              EXT2_FEATURE_RO_COMPAT_LARGE_FILE)) {
        return -1;
    }
    uint32_t log_block_size = u32_get(g_sb, 24);
    if (log_block_size > 2) {
        return -1;
    }
    g_block_size = 1024u << log_block_size;
    g_sectors_per_block = g_block_size / 512;
    g_blocks_count = u32_get(g_sb, 4);
    g_inodes_count = u32_get(g_sb, 0);
    g_blocks_per_group = u32_get(g_sb, 32);
    g_inodes_per_group = u32_get(g_sb, 40);
    g_first_data_block = u32_get(g_sb, 20);
    uint32_t rev_level = u32_get(g_sb, 76);
    g_inode_size = (rev_level >= 1) ? u16_get(g_sb, 88) : 128;
    if (g_inode_size < 128 || g_inode_size > 256) {
        return -1;
    }
    if (g_blocks_count == 0 || g_inodes_count == 0 || g_blocks_per_group == 0 ||
        g_inodes_per_group == 0) {
        return -1;
    }
    g_num_groups = (g_blocks_count + g_blocks_per_group - 1) / g_blocks_per_group;
    if (g_num_groups == 0 || g_num_groups > EXT2FS_MAX_GROUPS) {
        return -1;
    }
    g_dirent_file_type = (feature_incompat & EXT2_FEATURE_INCOMPAT_FILETYPE) != 0;
    g_gdt_start_byte = (uint64_t)(g_first_data_block + 1) * g_block_size;
    return journal_mount();
}
