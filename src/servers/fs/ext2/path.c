#include "ext2.h"

static uint32_t align4(uint32_t x) {
    return (x + 3) & ~3u;
}
static int find_in_dir(uint32_t dir_ino, const char *name, int name_len, uint32_t *out_ino,
                        uint32_t *out_size, int *out_is_dir) {
    uint8_t dir_inode[128];
    if (inode_io(dir_ino, dir_inode, 0) != 0) {
        return -1;
    }
    uint32_t size = u32_get(dir_inode, 4);
    uint32_t need_blocks = (size + g_block_size - 1) / g_block_size;
    static uint32_t blocks[EXT2FS_MAX_BLOCKS];
    uint32_t nblocks;
    if (walk_inode_blocks(dir_inode, need_blocks, blocks, &nblocks) != 0) {
        return -1;
    }
    static uint8_t dirbuf[EXT2FS_MAX_BLOCK_SIZE];
    for (uint32_t bi = 0; bi < nblocks; bi++) {
        if (block_read(blocks[bi], dirbuf) != 0) {
            return -1;
        }
        uint32_t off = 0;
        while (off + 8 <= g_block_size) {
            uint32_t ino = u32_get(dirbuf, off);
            uint16_t rec_len = u16_get(dirbuf, off + 4);
            uint8_t nl = dirbuf[off + 6];
            if (rec_len < 8) {
                break;
            }
            if (ino != 0 && nl == name_len) {
                int match = 1;
                for (int i = 0; i < name_len; i++) {
                    if (dirbuf[off + 8 + i] != (uint8_t)name[i]) {
                        match = 0;
                        break;
                    }
                }
                if (match) {
                    uint8_t fib[128];
                    if (inode_io(ino, fib, 0) != 0) {
                        return -1;
                    }
                    *out_ino = ino;
                    *out_size = u32_get(fib, 4);
                    *out_is_dir = ((u16_get(fib, 0) & 0xF000) == EXT2_S_IFDIR);
                    return 0;
                }
            }
            off += rec_len;
        }
    }
    return -1;
}

static char g_resolved_leaf[VFS_PATH_MAX];

static int copy_path(char dst[VFS_PATH_MAX], const char *src) {
    int i = 0;
    while (src[i] && i < VFS_PATH_MAX - 1) {
        dst[i] = src[i];
        i++;
    }
    if (src[i]) {
        return -1;
    }
    dst[i] = '\0';
    return 0;
}

static int is_root_relative_target(const char *target) {
    if (target[0] == '/') {
        return 1;
    }
    const char *roots[] = { "bin/", "sbin/", "usr/", "etc/", "var/", "boot/" };
    for (uint32_t i = 0; i < sizeof(roots) / sizeof(roots[0]); i++) {
        int j = 0;
        while (roots[i][j] && target[j] == roots[i][j]) {
            j++;
        }
        if (!roots[i][j]) {
            return 1;
        }
    }
    return 0;
}

static int replace_symlink(char work[VFS_PATH_MAX], int segment_start, const char *rest,
                           uint32_t ino) {
    uint8_t inode_buf[128];
    if (inode_io(ino, inode_buf, 0) != 0) {
        return -1;
    }
    uint32_t target_len = u32_get(inode_buf, 4);
    if (target_len == 0 || target_len > 60) {
        return -1;
    }
    char target[61];
    for (uint32_t i = 0; i < target_len; i++) {
        target[i] = (char)inode_buf[40 + i];
    }
    target[target_len] = '\0';

    int base_len = is_root_relative_target(target) ? 0 : segment_start;
    int target_start = target[0] == '/' ? 1 : 0;
    if (target[0] == '/' && EXT2FS_PREFIX_LEN != 0) {
        int match = 1;
        for (int i = 0; i < EXT2FS_PREFIX_LEN; i++) {
            if (target[i] != EXT2FS_MOUNT_PREFIX[i]) {
                match = 0;
                break;
            }
        }
        if (match) {
            target_start = EXT2FS_PREFIX_LEN;
        }
    }

    char combined[VFS_PATH_MAX];
    int n = 0;
    for (int i = 0; i < base_len; i++) {
        combined[n++] = work[i];
    }
    for (int i = target_start; target[i]; i++) {
        if (n >= VFS_PATH_MAX - 1) {
            return -1;
        }
        combined[n++] = target[i];
    }
    for (int i = 0; rest[i]; i++) {
        if (n >= VFS_PATH_MAX - 1) {
            return -1;
        }
        combined[n++] = rest[i];
    }
    combined[n] = '\0';
    return copy_path(work, combined);
}

static int resolve_path(const char *rel, int follow_final, uint32_t *out_parent_ino,
                        const char **out_leaf, int *out_leaf_len) {
    char work[VFS_PATH_MAX];
    if (copy_path(work, rel) != 0) {
        return -1;
    }
    for (int hop = 0; hop < 8; hop++) {
        uint32_t cur_ino = EXT2FS_ROOT_INODE;
        int p = 0;
        int replaced = 0;
        while (work[p]) {
            while (work[p] == '/') {
                p++;
            }
            if (!work[p]) {
                break;
            }
            int segment_start = p;
            while (work[p] && work[p] != '/') {
                p++;
            }
            int segment_len = p - segment_start;
            const char *rest = work + p;
            int is_final = rest[0] == '\0';
            uint32_t next_ino, next_size;
            int next_is_dir;
            if (find_in_dir(cur_ino, work + segment_start, segment_len, &next_ino, &next_size,
                            &next_is_dir) != 0) {
                if (is_final) {
                    for (int i = 0; i < segment_len; i++) {
                        g_resolved_leaf[i] = work[segment_start + i];
                    }
                    g_resolved_leaf[segment_len] = '\0';
                    *out_parent_ino = cur_ino;
                    *out_leaf = g_resolved_leaf;
                    *out_leaf_len = segment_len;
                    return 0;
                }
                return -1;
            }
            uint8_t inode_buf[128];
            if (inode_io(next_ino, inode_buf, 0) != 0) {
                return -1;
            }
            if ((u16_get(inode_buf, 0) & 0xF000) == EXT2_S_IFLNK &&
                (!is_final || follow_final)) {
                if (replace_symlink(work, segment_start, rest, next_ino) != 0) {
                    return -1;
                }
                replaced = 1;
                break;
            }
            if (is_final) {
                for (int i = 0; i < segment_len; i++) {
                    g_resolved_leaf[i] = work[segment_start + i];
                }
                g_resolved_leaf[segment_len] = '\0';
                *out_parent_ino = cur_ino;
                *out_leaf = g_resolved_leaf;
                *out_leaf_len = segment_len;
                return 0;
            }
            if (!next_is_dir) {
                return -2;
            }
            cur_ino = next_ino;
        }
        if (!replaced) {
            *out_parent_ino = EXT2FS_ROOT_INODE;
            g_resolved_leaf[0] = '\0';
            *out_leaf = g_resolved_leaf;
            *out_leaf_len = 0;
            return 0;
        }
    }
    return -1;
}

static int insert_dirent_in_dir(uint32_t parent_ino, uint32_t new_ino, const char *name,
                                 int name_len, uint8_t file_type) {
    uint32_t need_len = align4((uint32_t)(8 + name_len));
    uint8_t stored_file_type = g_dirent_file_type ? file_type : 0;
    uint8_t parent_inode[128];
    if (inode_io(parent_ino, parent_inode, 0) != 0) {
        return -1;
    }
    uint32_t size = u32_get(parent_inode, 4);
    uint32_t need_blocks = (size + g_block_size - 1) / g_block_size;
    static uint32_t blocks[EXT2FS_MAX_BLOCKS];
    uint32_t nblocks;
    if (walk_inode_blocks(parent_inode, need_blocks, blocks, &nblocks) != 0) {
        return -1;
    }
    static uint8_t dirbuf[EXT2FS_MAX_BLOCK_SIZE];
    for (uint32_t bi = 0; bi < nblocks; bi++) {
        if (block_read(blocks[bi], dirbuf) != 0) {
            return -1;
        }
        uint32_t off = 0;
        while (off + 8 <= g_block_size) {
            uint32_t ino = u32_get(dirbuf, off);
            uint16_t rec_len = u16_get(dirbuf, off + 4);
            uint8_t nl = dirbuf[off + 6];
            if (rec_len < 8) {
                break;
            }
            uint32_t ideal = (ino != 0) ? align4((uint32_t)(8 + nl)) : 0;
            uint32_t avail = rec_len - ideal;
            if (avail >= need_len) {
                if (ino != 0) {
                    u16_set(dirbuf, off + 4, (uint16_t)ideal);
                    uint32_t new_off = off + ideal;
                    u32_set(dirbuf, new_off, new_ino);
                    u16_set(dirbuf, new_off + 4, (uint16_t)(rec_len - ideal));
                    dirbuf[new_off + 6] = (uint8_t)name_len;
                    dirbuf[new_off + 7] = stored_file_type;
                    for (int i = 0; i < name_len; i++) {
                        dirbuf[new_off + 8 + i] = (uint8_t)name[i];
                    }
                } else {
                    u32_set(dirbuf, off, new_ino);
                    dirbuf[off + 6] = (uint8_t)name_len;
                    dirbuf[off + 7] = stored_file_type;
                    for (int i = 0; i < name_len; i++) {
                        dirbuf[off + 8 + i] = (uint8_t)name[i];
                    }
                }
                return block_write(blocks[bi], dirbuf);
            }
            off += rec_len;
        }
    }
    if (nblocks >= EXT2FS_MAX_BLOCKS) {
        return -1;
    }
    uint32_t new_block;
    if (alloc_block(&new_block) != 0) {
        return -1;
    }
    static uint8_t zerobuf[EXT2FS_MAX_BLOCK_SIZE];
    for (uint32_t i = 0; i < g_block_size; i++) {
        zerobuf[i] = 0;
    }
    u32_set(zerobuf, 0, new_ino);
    u16_set(zerobuf, 4, (uint16_t)g_block_size);
    zerobuf[6] = (uint8_t)name_len;
    zerobuf[7] = stored_file_type;
    for (int i = 0; i < name_len; i++) {
        zerobuf[8 + i] = (uint8_t)name[i];
    }
    if (block_write(new_block, zerobuf) != 0) {
        return -1;
    }
    if (append_block_to_inode(parent_inode, nblocks, new_block) != 0) {
        free_block(new_block);
        return -1;
    }
    u32_set(parent_inode, 4, size + g_block_size);
    u32_set(parent_inode, 28, compute_i_blocks_sectors(parent_inode, nblocks + 1));
    return inode_io(parent_ino, parent_inode, 1);
}
