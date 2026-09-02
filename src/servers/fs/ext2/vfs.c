#include "ext2.h"

static uint32_t ext2_now(void) {
    const volatile kinfo_page_t *k = kinfo_user();
    uint64_t hz = k->clock_hz ? k->clock_hz : 1;
    uint64_t now = kinfo_read_ticks(k) / hz;
    return now > 0xffffffffULL ? 0xffffffffU : (uint32_t)now;
}

static void handle_caps(msg_regs_t *m) {
    vfs_caps_reply_t *reply = (vfs_caps_reply_t *)m;
    reply->status = 0;
    reply->abi = ((uint64_t)ROBU_VFS_ABI_MAJOR << 32) | ROBU_VFS_ABI_MINOR;
    reply->features = ROBU_VFS_FEATURE_OPEN |
                      ROBU_VFS_FEATURE_READ |
                      ROBU_VFS_FEATURE_WRITE |
                      ROBU_VFS_FEATURE_STAT |
                      ROBU_VFS_FEATURE_READDIR |
                      ROBU_VFS_FEATURE_MUTATE |
                      ROBU_VFS_FEATURE_SYMLINK |
                      ROBU_VFS_FEATURE_XATTR |
                      ROBU_VFS_FEATURE_TIMESTAMPS |
                      ROBU_VFS_FEATURE_LINUX_VFS;
}

static void handle_open(msg_regs_t *m) {
    char path[VFS_PATH_MAX];
    const vfs_open_req_t *req = (const vfs_open_req_t *)m;
    uint64_t flags = req->flags;
    for (int i = 0; i < VFS_PATH_MAX; i++) {
        path[i] = req->name[i];
    }
    vfs_open_reply_t *reply = (vfs_open_reply_t *)m;

    const char *rel = path + EXT2FS_PREFIX_LEN;
    uint32_t parent_ino;
    const char *leaf;
    int leaf_len;
    int prc = resolve_path(rel, 1, &parent_ino, &leaf, &leaf_len);
    if (prc == -1) {
        reply->status = VFS_ERR_NOT_FOUND;
        return;
    }
    if (prc == -2) {
        reply->status = VFS_ERR_NOT_DIR;
        return;
    }

    uint32_t ino = 0, size = 0;
    int is_dir = 0;
    int found = find_in_dir(parent_ino, leaf, leaf_len, &ino, &size, &is_dir) == 0;

    if (!found) {
        if (!(flags & VFS_O_CREAT)) {
            reply->status = VFS_ERR_NOT_FOUND;
            return;
        }
        uint32_t new_ino;
        if (alloc_inode(&new_ino) != 0) {
            reply->status = VFS_ERR_NO_SPACE;
            return;
        }
        uint8_t inode_buf[128];
        for (int i = 0; i < 128; i++) {
            inode_buf[i] = 0;
        }
        u16_set(inode_buf, 0, EXT2_S_IFREG_0644);
        u16_set(inode_buf, 26, 1);
        uint32_t now = ext2_now();
        u32_set(inode_buf, 8, now);
        u32_set(inode_buf, 12, now);
        u32_set(inode_buf, 16, now);
        if (inode_io(new_ino, inode_buf, 1) != 0) {
            reply->status = VFS_ERR_NO_SPACE;
            return;
        }
        if (insert_dirent_in_dir(parent_ino, new_ino, leaf, leaf_len, EXT2_FT_REG_FILE) != 0) {
            reply->status = VFS_ERR_NO_SPACE;
            return;
        }
        ino = new_ino;
        size = 0;
    } else if (flags & VFS_O_TRUNC) {
        uint8_t inode_buf[128];
        if (inode_io(ino, inode_buf, 0) != 0) {
            reply->status = VFS_ERR_NOT_FOUND;
            return;
        }
        if (free_inode_blocks(inode_buf) != 0) {
            reply->status = VFS_ERR_NO_SPACE;
            return;
        }
        for (int i = 40; i < 40 + 60; i++) {
            inode_buf[i] = 0;
        }
        u32_set(inode_buf, 4, 0);
        u32_set(inode_buf, 28, compute_i_blocks_sectors(inode_buf, 0));
        uint32_t now = ext2_now();
        u32_set(inode_buf, 12, now);
        u32_set(inode_buf, 16, now);
        inode_io(ino, inode_buf, 1);
        size = 0;
    }

    int hidx = -1;
    for (int i = 0; i < EXT2FS_MAX_HANDLES; i++) {
        if (!handles[i].in_use) {
            hidx = i;
            break;
        }
    }
    if (hidx < 0) {
        reply->status = VFS_ERR_NO_SPACE;
        return;
    }
    ext2_handle_t *hd = &handles[hidx];
    hd->in_use = 1;
    hd->ino = ino;
    hd->file_size = size;
    hd->num_blocks = 0;
    hd->wcache_valid = 0;
    hd->wcache_dirty = 0;
    if (size > 0) {
        uint8_t inode_buf[128];
        if (inode_io(ino, inode_buf, 0) == 0) {
            uint32_t nblk = (size + g_block_size - 1) / g_block_size;
            walk_inode_blocks(inode_buf, nblk, hd->blocks, &hd->num_blocks);
        }
    }
    hd->offset = (flags & VFS_O_APPEND) ? size : 0;

    reply->status = 0;
    reply->handle = (uint64_t)hidx;
}

static void handle_read(msg_regs_t *m) {
    const vfs_read_req_t *req = (const vfs_read_req_t *)m;
    uint64_t h = req->handle;
    uint64_t len = req->len > VFS_READ_MAX ? VFS_READ_MAX : req->len;
    vfs_read_reply_t *reply = (vfs_read_reply_t *)m;
    if (h >= EXT2FS_MAX_HANDLES || !handles[h].in_use) {
        reply->status = VFS_ERR_BAD_HANDLE;
        return;
    }
    ext2_handle_t *hd = &handles[h];
    uint64_t avail = hd->file_size > hd->offset ? hd->file_size - hd->offset : 0;
    uint64_t n = len < avail ? len : avail;
    uint64_t got = 0;
    static uint8_t blockbuf[EXT2FS_MAX_BLOCK_SIZE];
    while (got < n) {
        uint64_t file_off = hd->offset + got;
        uint32_t blk_idx = (uint32_t)(file_off / g_block_size);
        uint32_t blk_off = (uint32_t)(file_off % g_block_size);
        if (blk_idx >= hd->num_blocks) {
            break;
        }
        if (block_read(hd->blocks[blk_idx], blockbuf) != 0) {
            break;
        }
        uint64_t chunk = g_block_size - blk_off;
        if (chunk > n - got) {
            chunk = n - got;
        }
        for (uint64_t i = 0; i < chunk; i++) {
            reply->data[got + i] = blockbuf[blk_off + i];
        }
        got += chunk;
    }
    hd->offset += got;
    reply->status = (int64_t)got;
}

static int flush_write_state(ext2_handle_t *hd) {
    if (!hd->wcache_valid || !hd->wcache_dirty) {
        return 0;
    }
    if (block_write(hd->blocks[hd->wcache_blk_idx], hd->wcache) != 0) {
        return -1;
    }
    uint8_t inode_buf[128];
    if (inode_io(hd->ino, inode_buf, 0) != 0) {
        return -1;
    }
    u32_set(inode_buf, 4, hd->file_size);
    u32_set(inode_buf, 28, compute_i_blocks_sectors(inode_buf, hd->num_blocks));
    uint32_t now = ext2_now();
    u32_set(inode_buf, 12, now);
    u32_set(inode_buf, 16, now);
    if (inode_io(hd->ino, inode_buf, 1) != 0) {
        return -1;
    }
    hd->wcache_dirty = 0;
    return 0;
}

static void handle_write(msg_regs_t *m) {
    const vfs_write_req_t *req = (const vfs_write_req_t *)m;
    uint64_t h = req->handle;
    uint64_t len = req->len > VFS_WRITE_MAX ? VFS_WRITE_MAX : req->len;
    uint8_t data[VFS_WRITE_MAX];
    for (uint64_t i = 0; i < len; i++) {
        data[i] = req->data[i];
    }
    vfs_write_reply_t *reply = (vfs_write_reply_t *)m;
    if (h >= EXT2FS_MAX_HANDLES || !handles[h].in_use) {
        reply->status = VFS_ERR_BAD_HANDLE;
        return;
    }
    ext2_handle_t *hd = &handles[h];
    uint64_t got = 0;
    while (got < len) {
        uint64_t file_off = hd->offset + got;
        uint32_t blk_idx = (uint32_t)(file_off / g_block_size);
        uint32_t blk_off = (uint32_t)(file_off % g_block_size);

        if (blk_idx >= hd->num_blocks) {
            if (hd->num_blocks >= EXT2FS_MAX_BLOCKS) {
                break;
            }
            uint32_t new_block;
            if (alloc_block(&new_block) != 0) {
                break;
            }
            uint8_t inode_buf[128];
            if (inode_io(hd->ino, inode_buf, 0) != 0) {
                free_block(new_block);
                break;
            }
            if (append_block_to_inode(inode_buf, hd->num_blocks, new_block) != 0) {
                free_block(new_block);
                break;
            }
            if (inode_io(hd->ino, inode_buf, 1) != 0) {
                break;
            }
            hd->blocks[hd->num_blocks++] = new_block;
        }

        if (!hd->wcache_valid || hd->wcache_blk_idx != blk_idx) {
            if (flush_write_state(hd) != 0) {
                break;
            }
            if (block_read(hd->blocks[blk_idx], hd->wcache) != 0) {
                break;
            }
            hd->wcache_valid = 1;
            hd->wcache_blk_idx = blk_idx;
        }

        uint64_t chunk = g_block_size - blk_off;
        if (chunk > len - got) {
            chunk = len - got;
        }
        for (uint64_t i = 0; i < chunk; i++) {
            hd->wcache[blk_off + i] = data[got + i];
        }
        hd->wcache_dirty = 1;
        got += chunk;
        if (hd->offset + got > hd->file_size) {
            hd->file_size = (uint32_t)(hd->offset + got);
        }
    }
    hd->offset += got;
    reply->status = (int64_t)got;
}

static void handle_close(msg_regs_t *m) {
    const vfs_close_req_t *req = (const vfs_close_req_t *)m;
    vfs_close_reply_t *reply = (vfs_close_reply_t *)m;
    uint64_t h = req->handle;
    if (h >= EXT2FS_MAX_HANDLES || !handles[h].in_use) {
        reply->status = VFS_ERR_BAD_HANDLE;
        return;
    }
    if (flush_write_state(&handles[h]) != 0) {
        reply->status = VFS_ERR_NO_SPACE;
        return;
    }
    handles[h].in_use = 0;
    handles[h].wcache_valid = 0;
    reply->status = 0;
}

static void handle_fstat(msg_regs_t *m) {
    const vfs_fstat_req_t *req = (const vfs_fstat_req_t *)m;
    vfs_stat_reply_t *reply = (vfs_stat_reply_t *)m;
    if (req->handle >= EXT2FS_MAX_HANDLES || !handles[req->handle].in_use) {
        reply->status = VFS_ERR_BAD_HANDLE;
        return;
    }
    ext2_handle_t *hd = &handles[req->handle];
    uint8_t inode_buf[128];
    if (inode_io(hd->ino, inode_buf, 0) != 0) {
        reply->status = VFS_ERR_NOT_FOUND;
        return;
    }
    reply->status = 0;
    reply->size = hd->file_size;
    reply->is_dir = (u16_get(inode_buf, 0) & 0xF000) == EXT2_S_IFDIR;
    reply->ino = hd->ino;
    reply->atime = u32_get(inode_buf, 8);
    reply->mtime = u32_get(inode_buf, 16);
}

static void handle_utimens(msg_regs_t *m) {
    const vfs_utimens_req_t *req = (const vfs_utimens_req_t *)m;
    vfs_utimens_reply_t *reply = (vfs_utimens_reply_t *)m;
    if (req->handle >= EXT2FS_MAX_HANDLES || !handles[req->handle].in_use ||
        (req->flags & ~(VFS_UTIME_OMIT_ATIME | VFS_UTIME_OMIT_MTIME)) != 0 ||
        req->atime < 0 || req->mtime < 0 || req->ctime < 0 ||
        req->atime > 0xffffffffLL || req->mtime > 0xffffffffLL || req->ctime > 0xffffffffLL) {
        reply->status = VFS_ERR_INVALID;
        return;
    }
    uint8_t inode_buf[128];
    if (inode_io(handles[req->handle].ino, inode_buf, 0) != 0) {
        reply->status = VFS_ERR_NOT_FOUND;
        return;
    }
    if (!(req->flags & VFS_UTIME_OMIT_ATIME)) {
        u32_set(inode_buf, 8, (uint32_t)req->atime);
    }
    if (!(req->flags & VFS_UTIME_OMIT_MTIME)) {
        u32_set(inode_buf, 16, (uint32_t)req->mtime);
    }
    u32_set(inode_buf, 12, (uint32_t)req->ctime);
    reply->status = inode_io(handles[req->handle].ino, inode_buf, 1) == 0 ? 0 : VFS_ERR_NO_SPACE;
}

static void handle_quiesce(msg_regs_t *m) {
    vfs_quiesce_reply_t *reply = (vfs_quiesce_reply_t *)m;
    for (int i = 0; i < EXT2FS_MAX_HANDLES; i++) {
        if (handles[i].in_use && flush_write_state(&handles[i]) != 0) {
            reply->status = VFS_ERR_NO_SPACE;
            return;
        }
    }
    reply->status = 0;
}

static void handle_stat(msg_regs_t *m) {
    char path[VFS_PATH_MAX];
    const vfs_stat_req_t *req = (const vfs_stat_req_t *)m;
    for (int i = 0; i < VFS_PATH_MAX; i++) {
        path[i] = req->name[i];
    }
    vfs_stat_reply_t *reply = (vfs_stat_reply_t *)m;

    const char *rel = path + EXT2FS_PREFIX_LEN;
    if (rel[0] == '\0') {
        uint8_t inode_buf[128];
        if (inode_io(EXT2FS_ROOT_INODE, inode_buf, 0) != 0) {
            reply->status = VFS_ERR_NOT_FOUND;
            return;
        }
        reply->status = 0;
        reply->size = 0;
        reply->is_dir = 1;
        reply->ino = EXT2FS_ROOT_INODE;
        reply->atime = u32_get(inode_buf, 8);
        reply->mtime = u32_get(inode_buf, 16);
        return;
    }
    uint32_t parent_ino;
    const char *leaf;
    int leaf_len;
    int prc = resolve_path(rel, 1, &parent_ino, &leaf, &leaf_len);
    if (prc == -1) {
        reply->status = VFS_ERR_NOT_FOUND;
        return;
    }
    if (prc == -2) {
        reply->status = VFS_ERR_NOT_DIR;
        return;
    }
    uint32_t ino, size;
    int is_dir;
    if (find_in_dir(parent_ino, leaf, leaf_len, &ino, &size, &is_dir) != 0) {
        reply->status = VFS_ERR_NOT_FOUND;
        return;
    }
    reply->status = 0;
    reply->size = size;
    reply->is_dir = is_dir ? 1 : 0;
    reply->ino = ino;
    uint8_t inode_buf[128];
    if (inode_io(ino, inode_buf, 0) != 0) {
        reply->status = VFS_ERR_NOT_FOUND;
        return;
    }
    reply->atime = u32_get(inode_buf, 8);
    reply->mtime = u32_get(inode_buf, 16);
}

static void handle_symlink(msg_regs_t *m) {
    char path[VFS_NAME_MAX];
    char target[VFS_NAME_MAX];
    const vfs_symlink_req_t *req = (const vfs_symlink_req_t *)m;
    for (int i = 0; i < VFS_NAME_MAX; i++) {
        path[i] = req->name[i];
        target[i] = req->target[i];
    }
    vfs_symlink_reply_t *reply = (vfs_symlink_reply_t *)m;

    const char *rel = path + EXT2FS_PREFIX_LEN;
    uint32_t parent_ino;
    const char *leaf;
    int leaf_len;
    int prc = resolve_path(rel, 0, &parent_ino, &leaf, &leaf_len);
    if (prc == -1) {
        reply->status = VFS_ERR_NOT_FOUND;
        return;
    }
    if (prc == -2) {
        reply->status = VFS_ERR_NOT_DIR;
        return;
    }

    uint32_t existing_ino, existing_size;
    int existing_is_dir;
    if (find_in_dir(parent_ino, leaf, leaf_len, &existing_ino, &existing_size, &existing_is_dir) == 0) {
        reply->status = VFS_ERR_EXISTS;
        return;
    }

    int target_len = 0;
    while (target[target_len] && target_len < VFS_NAME_MAX - 1) {
        target_len++;
    }
    if (target_len > 60) {
        reply->status = VFS_ERR_NO_SPACE;
        return;
    }

    uint32_t new_ino;
    if (alloc_inode(&new_ino) != 0) {
        reply->status = VFS_ERR_NO_SPACE;
        return;
    }
    uint8_t inode_buf[128];
    for (int i = 0; i < 128; i++) {
        inode_buf[i] = 0;
    }
    u16_set(inode_buf, 0, (uint16_t)(EXT2_S_IFLNK | 0777));
    u16_set(inode_buf, 26, 1);
    u32_set(inode_buf, 4, (uint32_t)target_len);
    for (int i = 0; i < target_len; i++) {
        inode_buf[40 + i] = (uint8_t)target[i];
    }
    if (inode_io(new_ino, inode_buf, 1) != 0) {
        reply->status = VFS_ERR_NO_SPACE;
        return;
    }

    if (insert_dirent_in_dir(parent_ino, new_ino, leaf, leaf_len, EXT2_FT_SYMLINK) != 0) {
        reply->status = VFS_ERR_NO_SPACE;
        return;
    }
    reply->status = 0;
}

static void handle_readdir(msg_regs_t *m) {
    const vfs_readdir_req_t *req = (const vfs_readdir_req_t *)m;
    uint64_t dir_ino = req->dir_ino;
    uint64_t want = req->index;
    vfs_readdir_reply_t *reply = (vfs_readdir_reply_t *)m;

    uint8_t dir_inode[128];
    if (inode_io((uint32_t)dir_ino, dir_inode, 0) != 0) {
        reply->status = VFS_ERR_NOT_FOUND;
        return;
    }
    uint32_t size = u32_get(dir_inode, 4);
    uint32_t need_blocks = (size + g_block_size - 1) / g_block_size;
    static uint32_t blocks[EXT2FS_MAX_BLOCKS];
    uint32_t nblocks;
    if (walk_inode_blocks(dir_inode, need_blocks, blocks, &nblocks) != 0) {
        reply->status = VFS_ERR_NOT_FOUND;
        return;
    }

    static uint8_t dirbuf[EXT2FS_MAX_BLOCK_SIZE];
    uint64_t seen = 0;
    for (uint32_t bi = 0; bi < nblocks; bi++) {
        if (block_read(blocks[bi], dirbuf) != 0) {
            break;
        }
        uint32_t off = 0;
        while (off + 8 <= g_block_size) {
            uint32_t ino = u32_get(dirbuf, off);
            uint16_t rec_len = u16_get(dirbuf, off + 4);
            uint8_t nl = dirbuf[off + 6];
            if (rec_len < 8) {
                break;
            }
            int is_dot = (nl == 1 && dirbuf[off + 8] == '.') ||
                         (nl == 2 && dirbuf[off + 8] == '.' && dirbuf[off + 9] == '.');
            if (ino != 0 && !is_dot) {
                if (seen == want) {
                    uint8_t entry_inode[128];
                    if (inode_io(ino, entry_inode, 0) != 0) {
                        reply->status = VFS_ERR_NOT_FOUND;
                        return;
                    }
                    reply->status = 0;
                    reply->is_dir = (u16_get(entry_inode, 0) & 0xF000) == EXT2_S_IFDIR;
                    int i = 0;
                    for (; i < VFS_NAME_MAX - 1 && i < nl; i++) {
                        reply->name[i] = (char)dirbuf[off + 8 + i];
                    }
                    reply->name[i] = '\0';
                    return;
                }
                seen++;
            }
            off += rec_len;
        }
    }
    reply->status = VFS_ERR_NOT_FOUND;
}

static int mkdir_internal(uint32_t parent_ino, const char *name, int name_len) {
    uint32_t existing_ino, existing_size;
    int existing_is_dir;
    if (find_in_dir(parent_ino, name, name_len, &existing_ino, &existing_size, &existing_is_dir) == 0) {
        return -3;
    }

    uint32_t new_ino;
    if (alloc_inode(&new_ino) != 0) {
        return -1;
    }
    uint32_t new_block;
    if (alloc_block(&new_block) != 0) {
        return -1;
    }
    static uint8_t dirbuf[EXT2FS_MAX_BLOCK_SIZE];
    for (uint32_t i = 0; i < g_block_size; i++) {
        dirbuf[i] = 0;
    }
    u32_set(dirbuf, 0, new_ino);
    u16_set(dirbuf, 4, 12);
    dirbuf[6] = 1;
    dirbuf[7] = g_dirent_file_type ? EXT2_FT_DIR : 0;
    dirbuf[8] = '.';
    u32_set(dirbuf, 12, parent_ino);
    u16_set(dirbuf, 16, (uint16_t)(g_block_size - 12));
    dirbuf[18] = 2;
    dirbuf[19] = g_dirent_file_type ? EXT2_FT_DIR : 0;
    dirbuf[20] = '.';
    dirbuf[21] = '.';
    if (block_write(new_block, dirbuf) != 0) {
        free_block(new_block);
        return -1;
    }

    uint8_t inode_buf[128];
    for (int i = 0; i < 128; i++) {
        inode_buf[i] = 0;
    }
    u16_set(inode_buf, 0, (uint16_t)(EXT2_S_IFDIR | 0755));
    u16_set(inode_buf, 26, 2);
    u32_set(inode_buf, 4, g_block_size);
    u32_set(inode_buf, 28, g_sectors_per_block);
    u32_set(inode_buf, 40, new_block);
    if (inode_io(new_ino, inode_buf, 1) != 0) {
        free_block(new_block);
        return -1;
    }

    if (insert_dirent_in_dir(parent_ino, new_ino, name, name_len, EXT2_FT_DIR) != 0) {
        return -1;
    }

    uint8_t parent_buf[128];
    if (inode_io(parent_ino, parent_buf, 0) == 0) {
        uint16_t links = u16_get(parent_buf, 26);
        u16_set(parent_buf, 26, (uint16_t)(links + 1));
        inode_io(parent_ino, parent_buf, 1);
    }

    uint32_t pg = (parent_ino - 1) / g_inodes_per_group;
    uint8_t pgd[32];
    if (gd_read(pg, pgd) == 0) {
        u16_set(pgd, 16, (uint16_t)(u16_get(pgd, 16) + 1));
        gd_write(pg, pgd);
    }

    return 0;
}

static void handle_mkdir(msg_regs_t *m) {
    char path[VFS_PATH_MAX];
    const vfs_mkdir_req_t *req = (const vfs_mkdir_req_t *)m;
    for (int i = 0; i < VFS_PATH_MAX; i++) {
        path[i] = req->name[i];
    }
    vfs_mkdir_reply_t *reply = (vfs_mkdir_reply_t *)m;

    const char *rel = path + EXT2FS_PREFIX_LEN;
    uint32_t parent_ino;
    const char *leaf;
    int leaf_len;
    int prc = resolve_path(rel, 0, &parent_ino, &leaf, &leaf_len);
    if (prc == -1) {
        reply->status = VFS_ERR_NOT_FOUND;
        return;
    }
    if (prc == -2) {
        reply->status = VFS_ERR_NOT_DIR;
        return;
    }

    int rc = mkdir_internal(parent_ino, leaf, leaf_len);
    if (rc == -3) {
        reply->status = VFS_ERR_EXISTS;
        return;
    }
    if (rc != 0) {
        reply->status = VFS_ERR_NO_SPACE;
        return;
    }
    reply->status = 0;
}

#define ROBU_SYS_INFO_CAT_SHM_GET 25
#define ROBU_SYS_INFO_CAT_SHM_AT  26
#define ROBU_SYS_INFO_CAT_SHM_DT  27
#define ROBU_SYS_INFO_CAT_SHM_CTL 28
#define ROBU_SYS_INFO_CAT_EXECREQ_REPLY 42
#define ROBU_IPC_FLAG_SYS_INFO (1u << 23)
#define ROBU_SHM_IPC_CREAT 01000
#define ROBU_SHM_IPC_RMID 0
#define ROBU_IPC_ERR_NONE 0
#define ROBU_IPC_ERR_NOT_FOUND (-1)
#define ROBU_IPC_ERR_INVALID (-9)

static int64_t bulk_shm_get_call(uint64_t size, int *out_id) {
    msg_regs_t m = {0};
    m.word[0] = ROBU_SYS_INFO_CAT_SHM_GET;
    m.word[1] = 0;
    m.word[2] = size;
    m.word[3] = ROBU_SHM_IPC_CREAT | 0600;
    int64_t rc = robu_ipc_raw(0, 0, ROBU_IPC_FLAG_SYS_INFO, &m, 0);
    if (rc == ROBU_IPC_ERR_NONE) {
        *out_id = (int)(int64_t)m.word[0];
    }
    return rc;
}

static int64_t bulk_shm_at_call(int shmid, uint64_t *out_va) {
    msg_regs_t m = {0};
    m.word[0] = ROBU_SYS_INFO_CAT_SHM_AT;
    m.word[1] = (uint64_t)(int64_t)shmid;
    m.word[2] = 0;
    m.word[3] = 0;
    int64_t rc = robu_ipc_raw(0, 0, ROBU_IPC_FLAG_SYS_INFO, &m, 0);
    if (rc == ROBU_IPC_ERR_NONE) {
        *out_va = m.word[0];
    }
    return rc;
}

static void bulk_shm_rmid_call(int shmid) {
    msg_regs_t m = {0};
    m.word[0] = ROBU_SYS_INFO_CAT_SHM_CTL;
    m.word[1] = (uint64_t)(int64_t)shmid;
    m.word[2] = ROBU_SHM_IPC_RMID;
    robu_ipc_raw(0, 0, ROBU_IPC_FLAG_SYS_INFO, &m, 0);
}

static void bulk_shm_dt_call(uint64_t va) {
    msg_regs_t m = {0};
    m.word[0] = ROBU_SYS_INFO_CAT_SHM_DT;
    m.word[1] = va;
    robu_ipc_raw(0, 0, ROBU_IPC_FLAG_SYS_INFO, &m, 0);
}

static void bulk_shm_release(int shmid, uint64_t va) {
    bulk_shm_rmid_call(shmid);
    bulk_shm_dt_call(va);
}

static void bulk_execreq_reply_call(uint64_t slot, uint64_t generation, int status,
                                     int shmid, uint64_t size) {
    msg_regs_t m = {0};
    m.word[0] = ROBU_SYS_INFO_CAT_EXECREQ_REPLY;
    m.word[1] = slot;
    m.word[2] = generation;
    m.word[3] = (uint64_t)(int64_t)status;
    m.word[4] = (uint64_t)(int64_t)shmid;
    m.word[5] = size;
    robu_ipc_raw(0, 0, ROBU_IPC_FLAG_SYS_INFO, &m, 0);
}

#define BULK_SEARCH_DIR_COUNT 5
static const char *const bulk_search_dirs[BULK_SEARCH_DIR_COUNT] = {
    "bin", "sbin", "usr/bin", "usr/sbin", "Core/Servers",
};
static uint32_t bulk_search_dir_ino[BULK_SEARCH_DIR_COUNT];
static int bulk_search_dirs_resolved;

static void bulk_resolve_search_dirs(void) {
    for (int i = 0; i < BULK_SEARCH_DIR_COUNT; i++) {
        uint32_t parent_ino;
        const char *leaf;
        int leaf_len;
        bulk_search_dir_ino[i] = 0;
        if (resolve_path(bulk_search_dirs[i], 1, &parent_ino, &leaf, &leaf_len) != 0) {
            continue;
        }
        uint32_t ino, size;
        int is_dir;
        if (find_in_dir(parent_ino, leaf, leaf_len, &ino, &size, &is_dir) == 0 && is_dir) {
            bulk_search_dir_ino[i] = ino;
        }
    }
    bulk_search_dirs_resolved = 1;
}

static int bulk_resolve_name(const char *name, int name_len, uint32_t *out_ino, uint32_t *out_size) {
    char qbuf[64];
    int qlen = name_len < 63 ? name_len : 63;
    for (int i = 0; i < qlen; i++) {
        qbuf[i] = name[i];
    }
    qbuf[qlen] = '\0';
    for (int hop = 0; hop < 8; hop++) {
        int found_symlink = 0;
        for (int d = 0; d < BULK_SEARCH_DIR_COUNT; d++) {
            if (bulk_search_dir_ino[d] == 0) {
                continue;
            }
            uint32_t ino, size;
            int is_dir;
            if (find_in_dir(bulk_search_dir_ino[d], qbuf, qlen, &ino, &size, &is_dir) != 0 ||
                is_dir) {
                continue;
            }
            uint8_t inode_buf[128];
            if (inode_io(ino, inode_buf, 0) != 0) {
                return -1;
            }
            uint16_t mode = u16_get(inode_buf, 0);
            if ((mode & 0xF000) == EXT2_S_IFLNK) {
                uint32_t tlen = u32_get(inode_buf, 4);
                if (tlen > 60) {
                    return -1;
                }
                char target[64];
                for (uint32_t i = 0; i < tlen; i++) {
                    target[i] = (char)inode_buf[40 + i];
                }
                target[tlen] = '\0';
                const char *base = target;
                for (const char *p = target; *p; p++) {
                    if (*p == '/') {
                        base = p + 1;
                    }
                }
                int blen = 0;
                while (base[blen]) {
                    blen++;
                }
                if (blen > 63) {
                    return -1;
                }
                for (int i = 0; i < blen; i++) {
                    qbuf[i] = base[i];
                }
                qbuf[blen] = '\0';
                qlen = blen;
                found_symlink = 1;
                break;
            }
            *out_ino = ino;
            *out_size = size;
            return 0;
        }
        if (!found_symlink) {
            return -1;
        }
    }
    return -1;
}

static void handle_read_bulk(msg_regs_t *m) {
    uint64_t slot = m->word[1];
    uint64_t generation = m->word[2];
    uint64_t namewords[3] = {m->word[3], m->word[4], m->word[5]};
    char name[25] = {0};
    for (int i = 0; i < 24; i++) {
        name[i] = ((const char *)namewords)[i];
    }
    int name_len = 0;
    while (name[name_len] && name_len < 24) {
        name_len++;
    }

    if (!bulk_search_dirs_resolved) {
        bulk_resolve_search_dirs();
    }

    uint32_t ino, size;
    if (bulk_resolve_name(name, name_len, &ino, &size) != 0 || size == 0) {
        bulk_execreq_reply_call(slot, generation, ROBU_IPC_ERR_NOT_FOUND, 0, 0);
        return;
    }

    int shmid;
    int64_t shm_get_rc = bulk_shm_get_call(size, &shmid);
    if (shm_get_rc != ROBU_IPC_ERR_NONE) {
        bulk_execreq_reply_call(slot, generation, (int)shm_get_rc, 0, 0);
        return;
    }
    uint64_t va;
    int64_t shm_at_rc = bulk_shm_at_call(shmid, &va);
    if (shm_at_rc != ROBU_IPC_ERR_NONE) {
        bulk_shm_rmid_call(shmid);
        bulk_execreq_reply_call(slot, generation, (int)shm_at_rc, 0, 0);
        return;
    }

    uint8_t inode_buf[128];
    if (inode_io(ino, inode_buf, 0) != 0) {
        bulk_shm_release(shmid, va);
        bulk_execreq_reply_call(slot, generation, ROBU_IPC_ERR_INVALID, 0, 0);
        return;
    }
    uint32_t need_blocks = (size + g_block_size - 1) / g_block_size;
    static uint32_t blocks[EXT2FS_MAX_BLOCKS];
    uint32_t nblocks;
    if (walk_inode_blocks(inode_buf, need_blocks, blocks, &nblocks) != 0) {
        bulk_shm_release(shmid, va);
        bulk_execreq_reply_call(slot, generation, ROBU_IPC_ERR_INVALID, 0, 0);
        return;
    }
    uint8_t *out = (uint8_t *)va;
    uint64_t remaining = size;
    for (uint32_t bi = 0; bi < nblocks && remaining > 0; bi++) {
        static uint8_t blockbuf[EXT2FS_MAX_BLOCK_SIZE];
        uint32_t chunk = remaining < g_block_size ? (uint32_t)remaining : g_block_size;
        if (blocks[bi] == 0) {
            for (uint32_t i = 0; i < chunk; i++) {
                out[i] = 0;
            }
        } else if (chunk == g_block_size) {
            uint32_t max_blocks = EXT2FS_BULK_READ_SECTORS / g_sectors_per_block;
            uint32_t run = 1;
            while (run < max_blocks && bi + run < nblocks &&
                   remaining >= (uint64_t)(run + 1) * g_block_size &&
                   blocks[bi + run] == blocks[bi] + run) {
                run++;
            }
            if (blkdev_read((uint64_t)blocks[bi] * g_sectors_per_block,
                            run * g_sectors_per_block, out) != 0) {
                bulk_shm_release(shmid, va);
                bulk_execreq_reply_call(slot, generation, ROBU_IPC_ERR_INVALID, 0, 0);
                return;
            }
            uint32_t bytes = run * g_block_size;
            out += bytes;
            remaining -= bytes;
            bi += run - 1;
            continue;
        } else {
            if (block_read(blocks[bi], blockbuf) != 0) {
                bulk_shm_release(shmid, va);
                bulk_execreq_reply_call(slot, generation, ROBU_IPC_ERR_INVALID, 0, 0);
                return;
            }
            for (uint32_t i = 0; i < chunk; i++) {
                out[i] = blockbuf[i];
            }
        }
        out += chunk;
        remaining -= chunk;
    }
    if (remaining != 0) {
        bulk_shm_release(shmid, va);
        bulk_execreq_reply_call(slot, generation, ROBU_IPC_ERR_INVALID, 0, 0);
        return;
    }

    bulk_execreq_reply_call(slot, generation, ROBU_IPC_ERR_NONE, shmid, size);
    bulk_shm_release(shmid, va);
}

#ifdef EXT2FS_AS_ROOT
static void seed_fixed_dirs(void) {
    static const char *const dirs[] = {
        "bin", "sbin", "etc", "usr", "usr/bin", "usr/sbin", "var", "var/tmp", "var/root",
    };
    for (uint32_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
        uint32_t parent_ino;
        const char *leaf;
        int leaf_len;
        if (resolve_path(dirs[i], 0, &parent_ino, &leaf, &leaf_len) != 0) {
            continue;
        }
        mkdir_internal(parent_ino, leaf, leaf_len);
    }
}
#endif
