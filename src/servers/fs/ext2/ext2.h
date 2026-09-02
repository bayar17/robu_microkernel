#ifndef ROBU_EXT2_H
#define ROBU_EXT2_H

#include "robu/types.h"
#include "robu/uipc.h"
#include "robu/ipc.h"
#include "robu/vfs.h"
#include "robu/kinfo.h"
#include "block.h"

#ifdef EXT2FS_AS_ROOT
#define EXT2FS_MOUNT_PREFIX "/"
#define EXT2FS_PREFIX_LEN 0
#else
#define EXT2FS_MOUNT_PREFIX "/mnt/ext2/"
#define EXT2FS_PREFIX_LEN 10
#endif

#define EXT2FS_MAX_HANDLES 8
#define EXT2FS_MAX_BLOCKS 4096
#define EXT2FS_MAX_BLOCK_SIZE 4096
#define EXT2FS_MAX_GROUPS 32
#define EXT2FS_BULK_READ_SECTORS 120
#define EXT2FS_ROOT_INODE 2
#define EXT2_FT_REG_FILE 1
#define EXT2_FT_DIR 2
#define EXT2_FT_SYMLINK 7
#define EXT2_S_IFDIR 0x4000
#define EXT2_S_IFLNK 0xA000
#define EXT2_S_IFREG_0644 0x81A4
#define EXT2_FEATURE_COMPAT_HAS_JOURNAL 0x0004
#define EXT2_FEATURE_COMPAT_EXT_ATTR 0x0008
#define EXT2_FEATURE_INCOMPAT_FILETYPE 0x0002
#define EXT2_FEATURE_RO_COMPAT_SPARSE_SUPER 0x0001
#define EXT2_FEATURE_RO_COMPAT_LARGE_FILE 0x0002
#define EXT2_I_FILE_ACL 104
#define EXT2_XATTR_MAGIC 0xEA020000
#define EXT2_XATTR_HEADER_SIZE 32
#define EXT2_XATTR_ENTRY_SIZE 16
#define EXT2_XATTR_INDEX_USER 1
#define EXT2FS_XATTR_MAX_ENTRIES 8

typedef struct {
    uint8_t name_len;
    uint8_t name[VFS_XATTR_NAME_MAX];
    uint32_t value_len;
    uint8_t value[VFS_XATTR_VALUE_MAX];
} ext2_xattr_entry_t;

typedef struct {
    int in_use;
    uint32_t ino;
    uint32_t blocks[EXT2FS_MAX_BLOCKS];
    uint32_t num_blocks;
    uint32_t file_size;
    uint64_t offset;
    int wcache_valid;
    int wcache_dirty;
    uint32_t wcache_blk_idx;
    uint8_t wcache[EXT2FS_MAX_BLOCK_SIZE];
} ext2_handle_t;

#endif
