#include "ext2.h"

#define EXT2_XATTR_SHM_STAT 28
#define EXT2_XATTR_SHM_AT 26
#define EXT2_XATTR_SHM_DT 27

static uint32_t xattr_align4(uint32_t value) {
    return (value + 3) & ~3u;
}

static int xattr_shm_size(int shmid, uint64_t *size_out) {
    msg_regs_t m = (msg_regs_t){0};
    m.word[0] = EXT2_XATTR_SHM_STAT;
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
    m.word[0] = EXT2_XATTR_SHM_AT;
    m.word[1] = (uint64_t)(int64_t)shmid;
    if (robu_ipc_raw(0, 0, IPC_FLAG_SYS_INFO, &m, NULL) != IPC_ERR_NONE) {
        return -1;
    }
    *addr_out = m.word[0];
    return 0;
}

static void xattr_shm_dt(uint64_t addr) {
    msg_regs_t m = (msg_regs_t){0};
    m.word[0] = EXT2_XATTR_SHM_DT;
    m.word[1] = addr;
    robu_ipc_raw(0, 0, IPC_FLAG_SYS_INFO, &m, NULL);
}

static int xattr_user_name(const uint8_t *name, uint64_t name_len, const uint8_t **suffix,
                           uint8_t *suffix_len) {
    static const uint8_t prefix[] = { 'u', 's', 'e', 'r', '.' };
    if (name_len <= sizeof(prefix)) {
        return -1;
    }
    for (uint32_t i = 0; i < sizeof(prefix); i++) {
        if (name[i] != prefix[i]) {
            return -1;
        }
    }
    uint64_t len = name_len - sizeof(prefix);
    if (len == 0 || len > VFS_XATTR_NAME_MAX) {
        return -1;
    }
    *suffix = name + sizeof(prefix);
    *suffix_len = (uint8_t)len;
    return 0;
}

static int xattr_entry_matches(const ext2_xattr_entry_t *entry, const uint8_t *name,
                               uint8_t name_len) {
    if (entry->name_len != name_len) {
        return 0;
    }
    for (uint32_t i = 0; i < name_len; i++) {
        if (entry->name[i] != name[i]) {
            return 0;
        }
    }
    return 1;
}

static int xattr_find_entry(const ext2_xattr_entry_t *entries, uint32_t count,
                            const uint8_t *name, uint8_t name_len) {
    for (uint32_t i = 0; i < count; i++) {
        if (xattr_entry_matches(&entries[i], name, name_len)) {
            return (int)i;
        }
    }
    return -1;
}

static void xattr_copy_entry(ext2_xattr_entry_t *dst, const ext2_xattr_entry_t *src) {
    dst->name_len = src->name_len;
    dst->value_len = src->value_len;
    for (uint32_t i = 0; i < src->name_len; i++) {
        dst->name[i] = src->name[i];
    }
    for (uint32_t i = 0; i < src->value_len; i++) {
        dst->value[i] = src->value[i];
    }
}

static int xattr_read_entries(const uint8_t inode_buf[128], ext2_xattr_entry_t *entries,
                              uint32_t *out_count, uint32_t *out_block, uint32_t *out_refcount) {
    uint32_t block = u32_get(inode_buf, EXT2_I_FILE_ACL);
    *out_count = 0;
    *out_block = block;
    *out_refcount = 0;
    if (block == 0) {
        return 0;
    }

    static uint8_t blockbuf[EXT2FS_MAX_BLOCK_SIZE];
    if (block_read(block, blockbuf) != 0 || u32_get(blockbuf, 0) != EXT2_XATTR_MAGIC ||
        u32_get(blockbuf, 8) != 1) {
        return -1;
    }
    uint32_t refcount = u32_get(blockbuf, 4);
    if (refcount == 0) {
        return -1;
    }

    uint32_t offset = EXT2_XATTR_HEADER_SIZE;
    uint32_t count = 0;
    for (;;) {
        if (offset + 4 > g_block_size) {
            return -1;
        }
        uint8_t name_len = blockbuf[offset];
        uint8_t name_index = blockbuf[offset + 1];
        if (name_len == 0 && name_index == 0) {
            break;
        }
        uint32_t entry_len = xattr_align4(EXT2_XATTR_ENTRY_SIZE + name_len);
        if (name_len == 0 || offset + entry_len > g_block_size || count >= EXT2FS_XATTR_MAX_ENTRIES ||
            name_index != EXT2_XATTR_INDEX_USER || u32_get(blockbuf, offset + 4) != 0) {
            return -2;
        }
        uint32_t value_len = u32_get(blockbuf, offset + 8);
        uint16_t value_off = u16_get(blockbuf, offset + 2);
        if (value_len > VFS_XATTR_VALUE_MAX ||
            (value_len != 0 && ((uint32_t)value_off < EXT2_XATTR_HEADER_SIZE ||
                                (uint32_t)value_off + value_len > g_block_size))) {
            return -2;
        }
        entries[count].name_len = name_len;
        entries[count].value_len = value_len;
        for (uint32_t i = 0; i < name_len; i++) {
            entries[count].name[i] = blockbuf[offset + EXT2_XATTR_ENTRY_SIZE + i];
        }
        for (uint32_t i = 0; i < value_len; i++) {
            entries[count].value[i] = blockbuf[(uint32_t)value_off + i];
        }
        count++;
        offset += entry_len;
    }
    *out_count = count;
    *out_refcount = refcount;
    return 0;
}

static int xattr_build_block(const ext2_xattr_entry_t *entries, uint32_t count,
                             uint8_t blockbuf[EXT2FS_MAX_BLOCK_SIZE]) {
    uint32_t entries_end = EXT2_XATTR_HEADER_SIZE + 4;
    for (uint32_t i = 0; i < count; i++) {
        if (entries[i].name_len == 0 || entries[i].value_len > VFS_XATTR_VALUE_MAX) {
            return -1;
        }
        entries_end += xattr_align4(EXT2_XATTR_ENTRY_SIZE + entries[i].name_len);
        if (entries_end > g_block_size) {
            return -1;
        }
    }

    uint32_t value_offsets[EXT2FS_XATTR_MAX_ENTRIES];
    uint32_t value_cursor = g_block_size;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t value_len = entries[i].value_len;
        if (value_len == 0) {
            value_offsets[i] = 0;
            continue;
        }
        uint32_t aligned_len = xattr_align4(value_len);
        if (value_cursor < aligned_len) {
            return -1;
        }
        value_cursor -= aligned_len;
        if (value_cursor < entries_end) {
            return -1;
        }
        value_offsets[i] = value_cursor;
    }

    for (uint32_t i = 0; i < g_block_size; i++) {
        blockbuf[i] = 0;
    }
    u32_set(blockbuf, 0, EXT2_XATTR_MAGIC);
    u32_set(blockbuf, 4, 1);
    u32_set(blockbuf, 8, 1);

    uint32_t offset = EXT2_XATTR_HEADER_SIZE;
    for (uint32_t i = 0; i < count; i++) {
        blockbuf[offset] = entries[i].name_len;
        blockbuf[offset + 1] = EXT2_XATTR_INDEX_USER;
        u16_set(blockbuf, offset + 2, (uint16_t)value_offsets[i]);
        u32_set(blockbuf, offset + 4, 0);
        u32_set(blockbuf, offset + 8, entries[i].value_len);
        for (uint32_t j = 0; j < entries[i].name_len; j++) {
            blockbuf[offset + EXT2_XATTR_ENTRY_SIZE + j] = entries[i].name[j];
        }
        for (uint32_t j = 0; j < entries[i].value_len; j++) {
            blockbuf[value_offsets[i] + j] = entries[i].value[j];
        }
        offset += xattr_align4(EXT2_XATTR_ENTRY_SIZE + entries[i].name_len);
    }
    return 0;
}

static int xattr_write_entries(uint32_t ino, uint8_t inode_buf[128],
                               const ext2_xattr_entry_t *entries, uint32_t count,
                               uint32_t old_block, uint32_t refcount) {
    if (count == 0) {
        if (old_block == 0) {
            return 0;
        }
        uint32_t blocks = u32_get(inode_buf, 28);
        if (blocks < g_sectors_per_block) {
            return -1;
        }
        u32_set(inode_buf, EXT2_I_FILE_ACL, 0);
        u32_set(inode_buf, 28, blocks - g_sectors_per_block);
        if (inode_io(ino, inode_buf, 1) != 0) {
            return -1;
        }
        return free_block(old_block);
    }
    if (old_block != 0 && refcount != 1) {
        return -2;
    }

    static uint8_t blockbuf[EXT2FS_MAX_BLOCK_SIZE];
    if (xattr_build_block(entries, count, blockbuf) != 0) {
        return -3;
    }

    if (old_block != 0) {
        return block_write(old_block, blockbuf);
    }

    uint32_t new_block;
    if (alloc_block(&new_block) != 0) {
        return -1;
    }
    if (block_write(new_block, blockbuf) != 0) {
        free_block(new_block);
        return -1;
    }
    u32_set(inode_buf, EXT2_I_FILE_ACL, new_block);
    u32_set(inode_buf, 28, u32_get(inode_buf, 28) + g_sectors_per_block);
    if (inode_io(ino, inode_buf, 1) != 0) {
        free_block(new_block);
        return -1;
    }
    u32_set(g_sb, 92, u32_get(g_sb, 92) | EXT2_FEATURE_COMPAT_EXT_ATTR);
    if (sb_write() != 0) {
        return -1;
    }
    return 0;
}

static int xattr_status_from_result(int result) {
    if (result == -2) {
        return VFS_ERR_NOT_SUPPORTED;
    }
    if (result == -3) {
        return VFS_ERR_NO_SPACE;
    }
    return VFS_ERR_NO_SPACE;
}

static void handle_xattr(msg_regs_t *m) {
    const vfs_xattr_req_t *req = (const vfs_xattr_req_t *)m;
    vfs_xattr_reply_t *reply = (vfs_xattr_reply_t *)m;
    uint64_t command = req->command & 0xffffffffULL;
    uint64_t flags = req->command >> VFS_XATTR_FLAGS_SHIFT;
    uint64_t name_len = req->name_len;
    uint64_t value_len = req->value_len;
    reply->value_len = 0;
    if (req->handle >= EXT2FS_MAX_HANDLES || !handles[req->handle].in_use || req->shmid < 0 ||
        name_len > VFS_XATTR_NAME_MAX || value_len > VFS_XATTR_VALUE_MAX ||
        (command != VFS_XATTR_GET && command != VFS_XATTR_SET && command != VFS_XATTR_LIST &&
         command != VFS_XATTR_REMOVE) ||
        ((command == VFS_XATTR_LIST && name_len != 0) ||
         (command != VFS_XATTR_LIST && name_len == 0)) ||
        ((command == VFS_XATTR_REMOVE) && value_len != 0)) {
        reply->status = VFS_ERR_INVALID;
        return;
    }
    if ((command != VFS_XATTR_SET && flags != 0) ||
        (flags & ~(uint64_t)(VFS_XATTR_CREATE | VFS_XATTR_REPLACE)) ||
        ((flags & VFS_XATTR_CREATE) && (flags & VFS_XATTR_REPLACE))) {
        reply->status = VFS_ERR_INVALID;
        return;
    }

    uint64_t shm_size;
    if (xattr_shm_size((int)req->shmid, &shm_size) != 0) {
        reply->status = VFS_ERR_NOT_FOUND;
        return;
    }
    uint64_t needed = command == VFS_XATTR_LIST ? value_len : name_len + value_len;
    if (shm_size < needed) {
        reply->status = VFS_ERR_INVALID;
        return;
    }
    uint64_t addr;
    if (xattr_shm_at((int)req->shmid, &addr) != 0) {
        reply->status = VFS_ERR_NOT_FOUND;
        return;
    }

    uint8_t *data = (uint8_t *)addr;
    const uint8_t *suffix = NULL;
    uint8_t suffix_len = 0;
    if (command != VFS_XATTR_LIST && xattr_user_name(data, name_len, &suffix, &suffix_len) != 0) {
        xattr_shm_dt(addr);
        reply->status = VFS_ERR_NOT_SUPPORTED;
        return;
    }

    ext2_handle_t *handle = &handles[req->handle];
    uint8_t inode_buf[128];
    ext2_xattr_entry_t entries[EXT2FS_XATTR_MAX_ENTRIES];
    uint32_t count;
    uint32_t block;
    uint32_t refcount;
    int load_result = inode_io(handle->ino, inode_buf, 0);
    if (load_result == 0) {
        load_result = xattr_read_entries(inode_buf, entries, &count, &block, &refcount);
    }
    if (load_result != 0) {
        xattr_shm_dt(addr);
        reply->status = load_result == -2 ? VFS_ERR_NOT_SUPPORTED : VFS_ERR_INVALID;
        return;
    }

    if (command == VFS_XATTR_GET) {
        int index = xattr_find_entry(entries, count, suffix, suffix_len);
        if (index < 0) {
            reply->status = VFS_ERR_NOT_FOUND;
        } else if (value_len != 0 && value_len < entries[index].value_len) {
            reply->status = VFS_ERR_NO_SPACE;
        } else if (value_len != 0 && shm_size < name_len + entries[index].value_len) {
            reply->status = VFS_ERR_INVALID;
        } else {
            if (value_len != 0) {
                for (uint32_t i = 0; i < entries[index].value_len; i++) {
                    data[name_len + i] = entries[index].value[i];
                }
            }
            reply->status = 0;
            reply->value_len = entries[index].value_len;
        }
    } else if (command == VFS_XATTR_LIST) {
        uint64_t total = 0;
        for (uint32_t i = 0; i < count; i++) {
            total += 5 + entries[i].name_len + 1;
        }
        if (value_len != 0 && value_len < total) {
            reply->status = VFS_ERR_NO_SPACE;
        } else if (value_len != 0 && shm_size < total) {
            reply->status = VFS_ERR_INVALID;
        } else {
            uint64_t offset = 0;
            if (value_len != 0) {
                for (uint32_t i = 0; i < count; i++) {
                    data[offset++] = 'u';
                    data[offset++] = 's';
                    data[offset++] = 'e';
                    data[offset++] = 'r';
                    data[offset++] = '.';
                    for (uint32_t j = 0; j < entries[i].name_len; j++) {
                        data[offset++] = entries[i].name[j];
                    }
                    data[offset++] = '\0';
                }
            }
            reply->status = 0;
            reply->value_len = total;
        }
    } else if (command == VFS_XATTR_SET) {
        int index = xattr_find_entry(entries, count, suffix, suffix_len);
        if (index >= 0 && (flags & VFS_XATTR_CREATE)) {
            reply->status = VFS_ERR_EXISTS;
        } else if (index < 0 && (flags & VFS_XATTR_REPLACE)) {
            reply->status = VFS_ERR_NOT_FOUND;
        } else {
            if (index < 0) {
                if (count >= EXT2FS_XATTR_MAX_ENTRIES) {
                    xattr_shm_dt(addr);
                    reply->status = VFS_ERR_NO_SPACE;
                    return;
                }
                index = (int)count++;
                entries[index].name_len = suffix_len;
                for (uint32_t i = 0; i < suffix_len; i++) {
                    entries[index].name[i] = suffix[i];
                }
            }
            entries[index].value_len = (uint32_t)value_len;
            for (uint32_t i = 0; i < value_len; i++) {
                entries[index].value[i] = data[name_len + i];
            }
            int result = xattr_write_entries(handle->ino, inode_buf, entries, count, block, refcount);
            reply->status = result == 0 ? 0 : xattr_status_from_result(result);
        }
    } else {
        int index = xattr_find_entry(entries, count, suffix, suffix_len);
        if (index < 0) {
            reply->status = VFS_ERR_NOT_FOUND;
        } else {
            for (uint32_t i = (uint32_t)index + 1; i < count; i++) {
                xattr_copy_entry(&entries[i - 1], &entries[i]);
            }
            count--;
            int result = xattr_write_entries(handle->ino, inode_buf, entries, count, block, refcount);
            reply->status = result == 0 ? 0 : xattr_status_from_result(result);
        }
    }
    xattr_shm_dt(addr);
}
