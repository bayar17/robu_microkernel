#ifndef ROBU_LINUX_VFS_H
#define ROBU_LINUX_VFS_H

#include "robu/types.h"
#include "robu/vfs.h"

#define ROBU_LINUX_VFS_ABI_MAJOR 1
#define ROBU_LINUX_VFS_ABI_MINOR 0

#define ROBU_LINUX_VFS_FEATURE_INODE   (1ULL << 0)
#define ROBU_LINUX_VFS_FEATURE_DENTRY  (1ULL << 1)
#define ROBU_LINUX_VFS_FEATURE_FILE    (1ULL << 2)
#define ROBU_LINUX_VFS_FEATURE_XATTR   (1ULL << 3)
#define ROBU_LINUX_VFS_FEATURE_ADAPTER (1ULL << 4)

#define ROBU_LINUX_VFS_NAME_MAX 255

typedef struct robu_linux_vfs_inode robu_linux_vfs_inode_t;
typedef struct robu_linux_vfs_dentry robu_linux_vfs_dentry_t;
typedef struct robu_linux_vfs_file robu_linux_vfs_file_t;
typedef struct robu_linux_vfs_superblock robu_linux_vfs_superblock_t;

typedef struct {
    int (*get)(robu_linux_vfs_inode_t *inode, const char *name, void *value, uint64_t size,
               uint64_t *value_size);
    int (*set)(robu_linux_vfs_inode_t *inode, const char *name, const void *value,
               uint64_t size, uint32_t flags);
    int (*list)(robu_linux_vfs_inode_t *inode, char *list, uint64_t size,
                uint64_t *list_size);
    int (*remove)(robu_linux_vfs_inode_t *inode, const char *name);
} robu_linux_vfs_xattr_ops_t;

struct robu_linux_vfs_inode {
    uint64_t ino;
    uint64_t size;
    uint32_t mode;
    uint32_t nlink;
    void *private_data;
    const robu_linux_vfs_xattr_ops_t *xattr_ops;
};

struct robu_linux_vfs_dentry {
    robu_linux_vfs_inode_t *inode;
    robu_linux_vfs_dentry_t *parent;
    char name[ROBU_LINUX_VFS_NAME_MAX + 1];
    uint32_t name_len;
};

struct robu_linux_vfs_file {
    robu_linux_vfs_dentry_t *dentry;
    uint64_t offset;
    uint32_t flags;
    void *private_data;
};

struct robu_linux_vfs_superblock {
    uint64_t block_size;
    uint64_t flags;
    robu_linux_vfs_dentry_t *root;
    void *private_data;
};

uint64_t robu_linux_vfs_features(void);
void robu_linux_vfs_inode_init(robu_linux_vfs_inode_t *inode, uint64_t ino, uint32_t mode,
                                void *private_data,
                                const robu_linux_vfs_xattr_ops_t *xattr_ops);
void robu_linux_vfs_dentry_init(robu_linux_vfs_dentry_t *dentry,
                                 robu_linux_vfs_dentry_t *parent,
                                 robu_linux_vfs_inode_t *inode,
                                 const char *name);

#endif
