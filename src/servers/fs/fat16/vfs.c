static void handle_caps(msg_regs_t *m) {
    vfs_caps_reply_t *reply = (vfs_caps_reply_t *)m;
    reply->status = 0;
    reply->abi = ((uint64_t)ROBU_VFS_ABI_MAJOR << 32) | ROBU_VFS_ABI_MINOR;
    reply->features = ROBU_VFS_FEATURE_OPEN |
                      ROBU_VFS_FEATURE_READ |
                      ROBU_VFS_FEATURE_WRITE |
                      ROBU_VFS_FEATURE_STAT |
                      ROBU_VFS_FEATURE_TIMESTAMPS;
}

static void handle_open(msg_regs_t *m) {
    char path[VFS_PATH_MAX];
    const vfs_open_req_t *req = (const vfs_open_req_t *)m;
    uint64_t flags = req->flags;
    for (int i = 0; i < VFS_PATH_MAX; i++) {
        path[i] = req->name[i];
    }
    vfs_open_reply_t *reply = (vfs_open_reply_t *)m;

    const char *rel = path + FAT16FS_PREFIX_LEN;
    uint8_t name83[11];
    to_83(rel, name83);

    uint16_t cluster = 0;
    uint32_t size = 0;
    uint32_t dirent_sector = 0, dirent_offset = 0;
    int found = find_in_root(name83, &cluster, &size, &dirent_sector, &dirent_offset) == 0;

    if (!found) {
        if (!(flags & VFS_O_CREAT)) {
            reply->status = VFS_ERR_NOT_FOUND;
            return;
        }
        if (find_free_root_slot(&dirent_sector, &dirent_offset) != 0) {
            reply->status = VFS_ERR_NO_SPACE;
            return;
        }
        if (write_new_dirent(dirent_sector, dirent_offset, name83) != 0) {
            reply->status = VFS_ERR_NO_SPACE;
            return;
        }
        cluster = 0;
        size = 0;
    } else if (flags & VFS_O_TRUNC) {
        if (cluster != 0) {
            free_chain(cluster);
        }
        cluster = 0;
        size = 0;
    }

    int hidx = -1;
    for (int i = 0; i < FAT16FS_MAX_HANDLES; i++) {
        if (!handles[i].in_use) {
            hidx = i;
            break;
        }
    }
    if (hidx < 0) {
        reply->status = VFS_ERR_NO_SPACE;
        return;
    }
    fat16_handle_t *hd = &handles[hidx];
    hd->in_use = 1;
    hd->file_size = size;
    hd->dirent_sector = dirent_sector;
    hd->dirent_offset = dirent_offset;
    hd->num_clusters = 0;
    if (size > 0 && cluster != 0) {
        if (walk_chain(cluster, hd) != 0) {
            hd->in_use = 0;
            reply->status = VFS_ERR_NOT_FOUND;
            return;
        }
    }
    hd->offset = (flags & VFS_O_APPEND) ? size : 0;

    if ((flags & VFS_O_TRUNC) && found) {
        update_dirent(hd);
    }

    reply->status = 0;
    reply->handle = (uint64_t)hidx;
}

static void handle_read(msg_regs_t *m) {
    const vfs_read_req_t *req = (const vfs_read_req_t *)m;
    uint64_t h = req->handle;
    uint64_t len = req->len > VFS_READ_MAX ? VFS_READ_MAX : req->len;
    vfs_read_reply_t *reply = (vfs_read_reply_t *)m;
    if (h >= FAT16FS_MAX_HANDLES || !handles[h].in_use) {
        reply->status = VFS_ERR_BAD_HANDLE;
        return;
    }
    fat16_handle_t *hd = &handles[h];
    uint64_t avail = hd->file_size > hd->offset ? hd->file_size - hd->offset : 0;
    uint64_t n = len < avail ? len : avail;
    uint64_t got = 0;
    static uint8_t sectorbuf[512];
    uint32_t cluster_size = (uint32_t)g_fat.sectors_per_cluster * g_fat.bytes_per_sector;
    while (got < n) {
        uint64_t file_off = hd->offset + got;
        uint32_t cluster_idx = (uint32_t)(file_off / cluster_size);
        uint32_t cluster_off = (uint32_t)(file_off % cluster_size);
        if (cluster_idx >= (uint32_t)hd->num_clusters) {
            break;
        }
        uint32_t sector_in_cluster = cluster_off / g_fat.bytes_per_sector;
        uint32_t sector_off = cluster_off % g_fat.bytes_per_sector;
        uint32_t sector = cluster_to_sector(hd->clusters[cluster_idx]) + sector_in_cluster;
        if (blkdev_read(sector, 1, sectorbuf) != 0) {
            break;
        }
        uint64_t chunk = g_fat.bytes_per_sector - sector_off;
        if (chunk > n - got) {
            chunk = n - got;
        }
        for (uint64_t i = 0; i < chunk; i++) {
            reply->data[got + i] = sectorbuf[sector_off + i];
        }
        got += chunk;
    }
    hd->offset += got;
    reply->status = (int64_t)got;
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
    if (h >= FAT16FS_MAX_HANDLES || !handles[h].in_use) {
        reply->status = VFS_ERR_BAD_HANDLE;
        return;
    }
    fat16_handle_t *hd = &handles[h];
    uint32_t cluster_size = (uint32_t)g_fat.sectors_per_cluster * g_fat.bytes_per_sector;
    uint64_t got = 0;
    static uint8_t sectorbuf[512];
    while (got < len) {
        uint64_t file_off = hd->offset + got;
        uint32_t cluster_idx = (uint32_t)(file_off / cluster_size);
        uint32_t cluster_off = (uint32_t)(file_off % cluster_size);

        if (cluster_idx >= (uint32_t)hd->num_clusters) {
            if (hd->num_clusters >= FAT16FS_MAX_CLUSTERS) {
                break;
            }
            uint32_t new_cluster;
            if (alloc_cluster(&new_cluster) != 0) {
                break;
            }
            if (hd->num_clusters > 0) {
                if (fat_write_entry(hd->clusters[hd->num_clusters - 1], (uint16_t)new_cluster) != 0) {
                    break;
                }
            }
            hd->clusters[hd->num_clusters++] = new_cluster;
        }

        uint32_t sector_in_cluster = cluster_off / g_fat.bytes_per_sector;
        uint32_t sector_off = cluster_off % g_fat.bytes_per_sector;
        uint32_t sector = cluster_to_sector(hd->clusters[cluster_idx]) + sector_in_cluster;
        uint64_t chunk = g_fat.bytes_per_sector - sector_off;
        if (chunk > len - got) {
            chunk = len - got;
        }

        if (sector_off != 0 || chunk != g_fat.bytes_per_sector) {
            if (blkdev_read(sector, 1, sectorbuf) != 0) {
                break;
            }
        }
        for (uint64_t i = 0; i < chunk; i++) {
            sectorbuf[sector_off + i] = data[got + i];
        }
        if (blkdev_write(sector, 1, sectorbuf) != 0) {
            break;
        }
        got += chunk;
    }
    hd->offset += got;
    if (hd->offset > hd->file_size) {
        hd->file_size = (uint32_t)hd->offset;
    }
    if (got > 0) {
        update_dirent(hd);
    }
    reply->status = (int64_t)got;
}

static void handle_close(msg_regs_t *m) {
    const vfs_close_req_t *req = (const vfs_close_req_t *)m;
    vfs_close_reply_t *reply = (vfs_close_reply_t *)m;
    uint64_t h = req->handle;
    if (h >= FAT16FS_MAX_HANDLES || !handles[h].in_use) {
        reply->status = VFS_ERR_BAD_HANDLE;
        return;
    }
    handles[h].in_use = 0;
    reply->status = 0;
}

static void handle_utimens(msg_regs_t *m) {
    const vfs_utimens_req_t *req = (const vfs_utimens_req_t *)m;
    vfs_utimens_reply_t *reply = (vfs_utimens_reply_t *)m;
    if (req->handle >= FAT16FS_MAX_HANDLES || !handles[req->handle].in_use ||
        (req->flags & ~(VFS_UTIME_OMIT_ATIME | VFS_UTIME_OMIT_MTIME)) != 0) {
        reply->status = VFS_ERR_INVALID;
        return;
    }
    reply->status = update_dirent_times(&handles[req->handle], req->atime, req->mtime, req->flags) == 0 ? 0 : VFS_ERR_NO_SPACE;
}

static void handle_stat(msg_regs_t *m) {
    char path[VFS_PATH_MAX];
    const vfs_stat_req_t *req = (const vfs_stat_req_t *)m;
    for (int i = 0; i < VFS_PATH_MAX; i++) {
        path[i] = req->name[i];
    }
    vfs_stat_reply_t *reply = (vfs_stat_reply_t *)m;

    const char *rel = path + FAT16FS_PREFIX_LEN;
    if (rel[0] == '\0') {
        reply->status = 0;
        reply->size = 0;
        reply->is_dir = 1;
        reply->ino = FAT16FS_ROOT_INO;
        return;
    }
    uint8_t name83[11];
    to_83(rel, name83);
    uint16_t cluster;
    uint32_t size;
    uint32_t dirent_sector, dirent_offset;
    if (find_in_root(name83, &cluster, &size, &dirent_sector, &dirent_offset) != 0) {
        reply->status = VFS_ERR_NOT_FOUND;
        return;
    }
    reply->status = 0;
    reply->size = size;
    reply->is_dir = 0;
    reply->ino = (uint64_t)cluster + 1;
    fat_dirent_t entry;
    if (read_dirent(dirent_sector, dirent_offset, &entry) != 0) {
        reply->status = VFS_ERR_NOT_FOUND;
        return;
    }
    reply->atime = fat_decode_time(entry.last_access_date, 0);
    reply->mtime = fat_decode_time(entry.write_date, entry.write_time);
}
