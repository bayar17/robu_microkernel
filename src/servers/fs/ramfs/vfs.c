static int64_t ramfs_now(void) {
    const volatile kinfo_page_t *k = kinfo_user();
    uint64_t hz = k->clock_hz ? k->clock_hz : 1;
    return (int64_t)(kinfo_read_ticks(k) / hz);
}

static int xattr_shm_size(int shmid, uint64_t *size_out) {
    msg_regs_t m = (msg_regs_t){0};
    m.word[0] = 28;
    m.word[1] = (uint64_t)(int64_t)shmid;
    m.word[2] = 2;
    if (robu_ipc_raw(0, 0, IPC_FLAG_SYS_INFO, &m, NULL) != IPC_ERR_NONE) {
        return -1;
    }
    *size_out = m.word[0];
    return 0;
}

static int xattr_shm_at(int shmid, uint64_t *addr_out) {
    msg_regs_t m = (msg_regs_t){0};
    m.word[0] = 26;
    m.word[1] = (uint64_t)(int64_t)shmid;
    if (robu_ipc_raw(0, 0, IPC_FLAG_SYS_INFO, &m, NULL) != IPC_ERR_NONE) {
        return -1;
    }
    *addr_out = m.word[0];
    return 0;
}

static void xattr_shm_dt(uint64_t addr) {
    msg_regs_t m = (msg_regs_t){0};
    m.word[0] = 27;
    m.word[1] = addr;
    robu_ipc_raw(0, 0, IPC_FLAG_SYS_INFO, &m, NULL);
}

static int xattr_name_eq(const ramfs_xattr_t *attr, const uint8_t *name, uint64_t name_len) {
    if (!attr->in_use || attr->name_len != name_len) {
        return 0;
    }
    for (uint64_t i = 0; i < name_len; i++) {
        if ((uint8_t)attr->name[i] != name[i]) {
            return 0;
        }
    }
    return 1;
}

static int xattr_find(ramfs_file_t *file, const uint8_t *name, uint64_t name_len) {
    for (int i = 0; i < RAMFS_XATTR_SLOTS; i++) {
        if (xattr_name_eq(&file->xattrs[i], name, name_len)) {
            return i;
        }
    }
    return -1;
}

static int xattr_alloc(ramfs_file_t *file) {
    for (int i = 0; i < RAMFS_XATTR_SLOTS; i++) {
        if (!file->xattrs[i].in_use) {
            return i;
        }
    }
    return -1;
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

static void handle_xattr(msg_regs_t *m) {
    const vfs_xattr_req_t *req = (const vfs_xattr_req_t *)m;
    vfs_xattr_reply_t *reply = (vfs_xattr_reply_t *)m;
    uint64_t command = req->command & 0xffffffffULL;
    uint64_t flags = req->command >> VFS_XATTR_FLAGS_SHIFT;
    uint64_t name_len = req->name_len;
    uint64_t value_len = req->value_len;
    if (!valid_handle(req->handle) || req->shmid < 0 ||
        name_len > VFS_XATTR_NAME_MAX || value_len > VFS_XATTR_VALUE_MAX) {
        reply->status = VFS_ERR_INVALID;
        return;
    }
    if ((command == VFS_XATTR_LIST && name_len != 0) ||
        (command != VFS_XATTR_LIST && name_len == 0)) {
        reply->status = VFS_ERR_INVALID;
        return;
    }
    uint64_t shm_size;
    if (xattr_shm_size((int)req->shmid, &shm_size) != 0) {
        reply->status = VFS_ERR_NOT_FOUND;
        return;
    }
    uint64_t minimum = command == VFS_XATTR_LIST ? value_len : name_len + value_len;
    if (shm_size < minimum) {
        reply->status = VFS_ERR_INVALID;
        return;
    }
    uint64_t addr;
    if (xattr_shm_at((int)req->shmid, &addr) != 0) {
        reply->status = VFS_ERR_NOT_FOUND;
        return;
    }
    uint8_t *data = (uint8_t *)addr;
    ramfs_file_t *file = &files[handles[req->handle].file_idx];
    if (command == VFS_XATTR_GET) {
        int index = xattr_find(file, data, name_len);
        if (index < 0) {
            reply->status = VFS_ERR_NOT_FOUND;
        } else if (value_len != 0 && value_len < file->xattrs[index].value_len) {
            reply->status = VFS_ERR_NO_SPACE;
        } else if (value_len != 0 && shm_size < name_len + file->xattrs[index].value_len) {
            reply->status = VFS_ERR_INVALID;
        } else {
            if (value_len != 0) {
                for (uint64_t i = 0; i < file->xattrs[index].value_len; i++) {
                    data[name_len + i] = file->xattrs[index].value[i];
                }
            }
            reply->status = 0;
            reply->value_len = file->xattrs[index].value_len;
        }
    } else if (command == VFS_XATTR_SET) {
        if (flags & ~(uint64_t)(VFS_XATTR_CREATE | VFS_XATTR_REPLACE)) {
            reply->status = VFS_ERR_INVALID;
        } else {
            int index = xattr_find(file, data, name_len);
            if (index >= 0 && (flags & VFS_XATTR_CREATE)) {
                reply->status = VFS_ERR_EXISTS;
            } else if (index < 0 && (flags & VFS_XATTR_REPLACE)) {
                reply->status = VFS_ERR_NOT_FOUND;
            } else {
                if (index < 0) {
                    index = xattr_alloc(file);
                }
                if (index < 0) {
                    reply->status = VFS_ERR_NO_SPACE;
                } else {
                    ramfs_xattr_t *attr = &file->xattrs[index];
                    attr->in_use = 1;
                    attr->name_len = (uint16_t)name_len;
                    attr->value_len = (uint16_t)value_len;
                    for (uint64_t i = 0; i < name_len; i++) {
                        attr->name[i] = (char)data[i];
                    }
                    attr->name[name_len] = '\0';
                    for (uint64_t i = 0; i < value_len; i++) {
                        attr->value[i] = data[name_len + i];
                    }
                    reply->status = 0;
                    reply->value_len = value_len;
                }
            }
        }
    } else if (command == VFS_XATTR_LIST) {
        uint64_t total = 0;
        for (int i = 0; i < RAMFS_XATTR_SLOTS; i++) {
            if (file->xattrs[i].in_use) {
                total += file->xattrs[i].name_len + 1;
            }
        }
        if (value_len != 0 && value_len < total) {
            reply->status = VFS_ERR_NO_SPACE;
        } else if (value_len != 0 && shm_size < total) {
            reply->status = VFS_ERR_INVALID;
        } else {
            uint64_t offset = 0;
            if (value_len != 0) {
                for (int i = 0; i < RAMFS_XATTR_SLOTS; i++) {
                    if (!file->xattrs[i].in_use) {
                        continue;
                    }
                    for (uint64_t j = 0; j < file->xattrs[i].name_len; j++) {
                        data[offset + j] = (uint8_t)file->xattrs[i].name[j];
                    }
                    offset += file->xattrs[i].name_len;
                    data[offset++] = '\0';
                }
            }
            reply->status = 0;
            reply->value_len = total;
        }
    } else if (command == VFS_XATTR_REMOVE) {
        int index = xattr_find(file, data, name_len);
        if (index < 0) {
            reply->status = VFS_ERR_NOT_FOUND;
        } else {
            file->xattrs[index].in_use = 0;
            reply->status = 0;
            reply->value_len = 0;
        }
    } else {
        reply->status = VFS_ERR_INVALID;
    }
    xattr_shm_dt(addr);
}

static void handle_open(msg_regs_t *m) {
    char name[VFS_PATH_MAX];
    const vfs_open_req_t *req = (const vfs_open_req_t *)m;
    uint64_t flags = req->flags;
    for (int i = 0; i < VFS_PATH_MAX; i++) name[i] = req->name[i];
    vfs_open_reply_t *reply = (vfs_open_reply_t *)m;

    int raw_idx = find_file(name);
    int fidx = (raw_idx >= 0 && files[raw_idx].is_symlink) ? resolve_path(name) : raw_idx;
    if (fidx < 0) {
        if (raw_idx >= 0 || !(flags & VFS_O_CREAT)) { reply->status = VFS_ERR_NOT_FOUND; return; }
        fidx = alloc_file();
        files[fidx].in_use = 1;
        files[fidx].is_dir = 0;
        files[fidx].is_symlink = 0;
        files[fidx].size = 0;
        files[fidx].capacity = 0;
        files[fidx].data = NULL;
        int64_t now = ramfs_now();
        files[fidx].atime = now;
        files[fidx].mtime = now;
        files[fidx].ctime = now;
        xattrs_reset(&files[fidx]);
        set_name(files[fidx].name, name, sizeof(files[fidx].name));
        files[fidx].parent_ino = resolve_parent_ino(name);
    } else if (flags & VFS_O_TRUNC) {
        files[fidx].size = 0;
        int64_t now = ramfs_now();
        files[fidx].mtime = now;
        files[fidx].ctime = now;
    }
    int hidx = alloc_handle();
    handles[hidx].in_use = 1;
    handles[hidx].file_idx = fidx;
    handles[hidx].offset = (flags & VFS_O_APPEND) ? files[fidx].size : 0;
    reply->status = 0;
    reply->handle = (uint64_t)hidx;
}

static void handle_read(msg_regs_t *m) {
    const vfs_read_req_t *req = (const vfs_read_req_t *)m;
    uint64_t h = req->handle;
    uint64_t len = req->len > VFS_READ_MAX ? VFS_READ_MAX : req->len;
    vfs_read_reply_t *reply = (vfs_read_reply_t *)m;
    if (!valid_handle(h)) { reply->status = VFS_ERR_BAD_HANDLE; return; }

    ramfs_handle_t *hd = &handles[h];
    ramfs_file_t *f = &files[hd->file_idx];

    uint64_t avail = f->size > hd->offset ? f->size - hd->offset : 0;
    uint64_t n = len < avail ? len : avail;
    if (f->data) {
        for (uint64_t i = 0; i < n; i++) reply->data[i] = f->data[hd->offset + i];
    }
    hd->offset += n;
    reply->status = (int64_t)n;
}

static void handle_write(msg_regs_t *m) {
    const vfs_write_req_t *req = (const vfs_write_req_t *)m;
    uint64_t h = req->handle;
    uint64_t len = req->len > VFS_WRITE_MAX ? VFS_WRITE_MAX : req->len;
    uint8_t data[VFS_WRITE_MAX];
    for (uint64_t i = 0; i < len; i++) data[i] = req->data[i];
    vfs_write_reply_t *reply = (vfs_write_reply_t *)m;
    if (!valid_handle(h)) { reply->status = VFS_ERR_BAD_HANDLE; return; }

    ramfs_handle_t *hd = &handles[h];
    ramfs_file_t *f = &files[hd->file_idx];

    if (hd->offset + len > f->capacity) {
        uint64_t new_cap = f->capacity == 0 ? 4096 : f->capacity * 2;
        while (new_cap < hd->offset + len) new_cap *= 2;

        uint8_t *new_data = (uint8_t*)ram_alloc(new_cap);
        if (f->data && f->size > 0) {
            uint64_t *src64 = (uint64_t*)f->data;
            uint64_t *dst64 = (uint64_t*)new_data;
            uint64_t words = f->size / 8;
            for (uint64_t i = 0; i < words; i++) dst64[i] = src64[i];
            for (uint64_t i = words * 8; i < f->size; i++) new_data[i] = f->data[i];
        }
        f->data = new_data;
        f->capacity = new_cap;
    }

    for (uint64_t i = 0; i < len; i++) {
        f->data[hd->offset + i] = data[i];
    }
    hd->offset += len;
    if (hd->offset > f->size) f->size = hd->offset;
    int64_t now = ramfs_now();
    f->mtime = now;
    f->ctime = now;
    reply->status = (int64_t)len;
}

static void handle_close(msg_regs_t *m) {
    const vfs_close_req_t *req = (const vfs_close_req_t *)m;
    vfs_close_reply_t *reply = (vfs_close_reply_t *)m;
    if (!valid_handle(req->handle)) { reply->status = VFS_ERR_BAD_HANDLE; return; }
    handles[req->handle].in_use = 0;
    reply->status = 0;
}

static void handle_utimens(msg_regs_t *m) {
    const vfs_utimens_req_t *req = (const vfs_utimens_req_t *)m;
    vfs_utimens_reply_t *reply = (vfs_utimens_reply_t *)m;
    if (!valid_handle(req->handle) ||
        (req->flags & ~(VFS_UTIME_OMIT_ATIME | VFS_UTIME_OMIT_MTIME)) != 0) {
        reply->status = VFS_ERR_INVALID;
        return;
    }
    ramfs_file_t *file = &files[handles[req->handle].file_idx];
    if (!(req->flags & VFS_UTIME_OMIT_ATIME)) file->atime = req->atime;
    if (!(req->flags & VFS_UTIME_OMIT_MTIME)) file->mtime = req->mtime;
    file->ctime = req->ctime;
    reply->status = 0;
}

static void handle_stat(msg_regs_t *m) {
    char name[VFS_PATH_MAX];
    const vfs_stat_req_t *req = (const vfs_stat_req_t *)m;
    for (int i = 0; i < VFS_PATH_MAX; i++) name[i] = req->name[i];
    vfs_stat_reply_t *reply = (vfs_stat_reply_t *)m;
    if (name[0] == '/' && name[1] == '\0') {
        reply->status = 0; reply->size = 0; reply->is_dir = 1; reply->ino = VFS_ROOT_INO;
        reply->atime = 0; reply->mtime = 0;
        return;
    }
    int raw_idx = find_file(name);
    int fidx = (raw_idx >= 0 && files[raw_idx].is_symlink) ? resolve_path(name) : raw_idx;
    if (fidx < 0) { reply->status = VFS_ERR_NOT_FOUND; return; }
    reply->status = 0;
    reply->size = files[fidx].size;
    reply->is_dir = (uint64_t)files[fidx].is_dir;
    reply->ino = (uint64_t)fidx + 2;
    reply->atime = files[fidx].atime;
    reply->mtime = files[fidx].mtime;
}

static void handle_fstat(msg_regs_t *m) {
    const vfs_fstat_req_t *req = (const vfs_fstat_req_t *)m;
    uint64_t h = req->handle;
    vfs_stat_reply_t *reply = (vfs_stat_reply_t *)m;
    if (!valid_handle(h)) { reply->status = VFS_ERR_BAD_HANDLE; return; }
    reply->status = 0;
    reply->size = files[handles[h].file_idx].size;
    reply->is_dir = (uint64_t)files[handles[h].file_idx].is_dir;
    reply->ino = (uint64_t)handles[h].file_idx + 2;
    reply->atime = files[handles[h].file_idx].atime;
    reply->mtime = files[handles[h].file_idx].mtime;
}

static void handle_readdir(msg_regs_t *m) {
    const vfs_readdir_req_t *req = (const vfs_readdir_req_t *)m;
    uint64_t want = req->index;
    vfs_readdir_reply_t *reply = (vfs_readdir_reply_t *)m;
    uint64_t seen = 0;
    for (uint32_t i = 0; i < max_files; i++) {
        if (!files[i].in_use || files[i].parent_ino != req->dir_ino) continue;
        if (seen == want) {
            reply->status = 0;
            reply->is_dir = (uint64_t)files[i].is_dir;
            set_name(reply->name, basename_of(files[i].name), sizeof(reply->name));
            return;
        }
        seen++;
    }
    reply->status = VFS_ERR_NOT_FOUND;
}

static void handle_rename(msg_regs_t *m) {
    char oldname[VFS_NAME_MAX], newname[VFS_NAME_MAX];
    const vfs_rename_req_t *req = (const vfs_rename_req_t *)m;
    for (int i = 0; i < VFS_NAME_MAX; i++) {
        oldname[i] = req->oldname[i]; newname[i] = req->newname[i];
    }
    vfs_rename_reply_t *reply = (vfs_rename_reply_t *)m;
    int fidx = find_file(oldname);
    if (fidx < 0) { reply->status = VFS_ERR_NOT_FOUND; return; }
    if (files[fidx].is_dir) { reply->status = VFS_ERR_IS_DIR; return; }
    int existing = find_file(newname);
    if (existing >= 0) {
        if (files[existing].is_dir) { reply->status = VFS_ERR_IS_DIR; return; }
        if (existing != fidx) files[existing].in_use = 0;
    }
    set_name(files[fidx].name, newname, sizeof(files[fidx].name));
    files[fidx].parent_ino = resolve_parent_ino(newname);
    reply->status = 0;
}

static void handle_unlink(msg_regs_t *m) {
    char name[VFS_PATH_MAX];
    const vfs_unlink_req_t *req = (const vfs_unlink_req_t *)m;
    for (int i = 0; i < VFS_PATH_MAX; i++) name[i] = req->name[i];
    vfs_unlink_reply_t *reply = (vfs_unlink_reply_t *)m;
    int fidx = find_file(name);
    if (fidx < 0) { reply->status = VFS_ERR_NOT_FOUND; return; }
    if (files[fidx].is_dir) { reply->status = VFS_ERR_IS_DIR; return; }
    files[fidx].in_use = 0;
    reply->status = 0;
}

static void handle_symlink(msg_regs_t *m) {
    char name[VFS_NAME_MAX], target[VFS_NAME_MAX];
    const vfs_symlink_req_t *req = (const vfs_symlink_req_t *)m;
    for (int i = 0; i < VFS_NAME_MAX; i++) {
        name[i] = req->name[i]; target[i] = req->target[i];
    }
    vfs_symlink_reply_t *reply = (vfs_symlink_reply_t *)m;
    if (find_file(name) >= 0) { reply->status = VFS_ERR_NOT_SUPPORTED; return; }
    int idx = alloc_file();
    files[idx].in_use = 1;
    files[idx].is_dir = 0;
    files[idx].is_symlink = 1;
    xattrs_reset(&files[idx]);
    files[idx].size = 0;
    set_name(files[idx].name, name, sizeof(files[idx].name));
    set_name(files[idx].target, target, sizeof(files[idx].target));
    files[idx].parent_ino = resolve_parent_ino(name);
    reply->status = 0;
}

static void handle_mkdir(msg_regs_t *m) {
    char name[VFS_PATH_MAX];
    const vfs_mkdir_req_t *req = (const vfs_mkdir_req_t *)m;
    for (int i = 0; i < VFS_PATH_MAX; i++) name[i] = req->name[i];
    vfs_mkdir_reply_t *reply = (vfs_mkdir_reply_t *)m;
    if (find_file(name) >= 0) { reply->status = VFS_ERR_EXISTS; return; }
    uint64_t parent_ino;
    if (!resolve_parent_checked(name, &parent_ino)) { reply->status = VFS_ERR_NOT_FOUND; return; }
    int idx = alloc_file();
    files[idx].in_use = 1;
    files[idx].is_dir = 1;
    files[idx].is_symlink = 0;
    files[idx].size = 0;
    files[idx].capacity = 0;
    files[idx].data = NULL;
    xattrs_reset(&files[idx]);
    set_name(files[idx].name, name, sizeof(files[idx].name));
    files[idx].parent_ino = parent_ino;
    reply->status = 0;
}

static void handle_rmdir(msg_regs_t *m) {
    char name[VFS_PATH_MAX];
    const vfs_rmdir_req_t *req = (const vfs_rmdir_req_t *)m;
    for (int i = 0; i < VFS_PATH_MAX; i++) name[i] = req->name[i];
    vfs_rmdir_reply_t *reply = (vfs_rmdir_reply_t *)m;
    int fidx = find_file(name);
    if (fidx < 0) { reply->status = VFS_ERR_NOT_FOUND; return; }
    if (!files[fidx].is_dir) { reply->status = VFS_ERR_NOT_DIR; return; }
    uint64_t ino = (uint64_t)fidx + 2;
    for (uint32_t i = 0; i < max_files; i++) {
        if (files[i].in_use && files[i].parent_ino == ino) { reply->status = VFS_ERR_NOT_EMPTY; return; }
    }
    files[fidx].in_use = 0;
    reply->status = 0;
}

static void handle_link(msg_regs_t *m) {
    char oldname[VFS_NAME_MAX], newname[VFS_NAME_MAX];
    const vfs_link_req_t *req = (const vfs_link_req_t *)m;
    for (int i = 0; i < VFS_NAME_MAX; i++) {
        oldname[i] = req->oldname[i]; newname[i] = req->newname[i];
    }
    vfs_link_reply_t *reply = (vfs_link_reply_t *)m;
    int raw_idx = find_file(oldname);
    int fidx = (raw_idx >= 0 && files[raw_idx].is_symlink) ? resolve_path(oldname) : raw_idx;
    if (fidx < 0) { reply->status = VFS_ERR_NOT_FOUND; return; }
    if (files[fidx].is_dir) { reply->status = VFS_ERR_IS_DIR; return; }
    if (find_file(newname) >= 0) { reply->status = VFS_ERR_EXISTS; return; }
    uint64_t parent_ino;
    if (!resolve_parent_checked(newname, &parent_ino)) { reply->status = VFS_ERR_NOT_FOUND; return; }
    uint64_t size = files[fidx].size;
    uint8_t *srcdata = files[fidx].data;
    int idx = alloc_file();
    files[idx].in_use = 1;
    files[idx].is_dir = 0;
    files[idx].is_symlink = 0;
    files[idx].size = size;
    files[idx].capacity = size;
    files[idx].data = size ? (uint8_t*)ram_alloc(size) : NULL;
    xattrs_reset(&files[idx]);
    for (uint64_t i = 0; i < size; i++) files[idx].data[i] = srcdata[i];
    set_name(files[idx].name, newname, sizeof(files[idx].name));
    files[idx].parent_ino = parent_ino;
    reply->status = 0;
}

static void handle_mknod(msg_regs_t *m) {
    char name[VFS_NAME_MAX];
    const vfs_mknod_req_t *req = (const vfs_mknod_req_t *)m;
    for (int i = 0; i < VFS_NAME_MAX; i++) name[i] = req->name[i];
    vfs_mknod_reply_t *reply = (vfs_mknod_reply_t *)m;
    if (find_file(name) >= 0) { reply->status = VFS_ERR_EXISTS; return; }
    uint64_t parent_ino;
    if (!resolve_parent_checked(name, &parent_ino)) { reply->status = VFS_ERR_NOT_FOUND; return; }
    int idx = alloc_file();
    files[idx].in_use = 1;
    files[idx].is_dir = 0;
    files[idx].is_symlink = 0;
    files[idx].size = 0;
    files[idx].capacity = 0;
    files[idx].data = NULL;
    xattrs_reset(&files[idx]);
    set_name(files[idx].name, name, sizeof(files[idx].name));
    files[idx].parent_ino = parent_ino;
    reply->status = 0;
}

static uint64_t seed_dir(const char *name, uint64_t parent_ino) {
    int idx = alloc_file();
    files[idx].in_use = 1;
    files[idx].is_dir = 1;
    files[idx].is_symlink = 0;
    files[idx].parent_ino = parent_ino;
    files[idx].size = 0;
    xattrs_reset(&files[idx]);
    set_name(files[idx].name, name, sizeof(files[idx].name));
    return (uint64_t)idx + 2;
}

static void seed_fixed_dirs(void) {
    seed_dir("bin", VFS_ROOT_INO);
    seed_dir("etc", VFS_ROOT_INO);
    seed_dir("tmp", VFS_ROOT_INO);
    uint64_t var_ino = seed_dir("var", VFS_ROOT_INO);
    if (var_ino) {
        seed_dir("var/tmp", var_ino);
        seed_dir("var/root", var_ino);
        seed_dir("var/lock", var_ino);
        seed_dir("var/lib", var_ino);
        seed_dir("var/run", var_ino);
    }
    seed_dir("sbin", VFS_ROOT_INO);
    uint64_t usr_ino = seed_dir("usr", VFS_ROOT_INO);
    if (usr_ino) {
        seed_dir("usr/bin", usr_ino);
        seed_dir("usr/sbin", usr_ino);
    }
    seed_dir("dev", VFS_ROOT_INO);
    seed_dir("proc", VFS_ROOT_INO);
}
