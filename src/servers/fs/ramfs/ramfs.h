#ifndef ROBU_RAMFS_H
#define ROBU_RAMFS_H

#include "robu/types.h"
#include "robu/uipc.h"
#include "robu/vfs.h"

#define RAMFS_XATTR_SLOTS 8

typedef struct {
    int in_use;
    uint16_t name_len;
    uint16_t value_len;
    char name[VFS_XATTR_NAME_MAX + 1];
    uint8_t value[VFS_XATTR_VALUE_MAX];
} ramfs_xattr_t;

typedef struct {
    int in_use;
    int is_dir;
    int is_symlink;
    uint64_t parent_ino;
    char name[VFS_PATH_MAX];
    char target[VFS_PATH_MAX];
    uint64_t size;
    uint64_t capacity;
    uint8_t *data;
    int64_t atime;
    int64_t mtime;
    int64_t ctime;
    ramfs_xattr_t xattrs[RAMFS_XATTR_SLOTS];
} ramfs_file_t;

typedef struct {
    int in_use;
    int file_idx;
    uint64_t offset;
} ramfs_handle_t;

#endif
