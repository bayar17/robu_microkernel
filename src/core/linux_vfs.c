#include "robu/linux_vfs.h"

uint64_t robu_linux_vfs_features(void) {
    return ROBU_LINUX_VFS_FEATURE_INODE |
           ROBU_LINUX_VFS_FEATURE_DENTRY |
           ROBU_LINUX_VFS_FEATURE_FILE |
           ROBU_LINUX_VFS_FEATURE_XATTR |
           ROBU_LINUX_VFS_FEATURE_ADAPTER;
}

void robu_linux_vfs_inode_init(robu_linux_vfs_inode_t *inode, uint64_t ino, uint32_t mode,
                                void *private_data,
                                const robu_linux_vfs_xattr_ops_t *xattr_ops) {
    inode->ino = ino;
    inode->size = 0;
    inode->mode = mode;
    inode->nlink = 1;
    inode->private_data = private_data;
    inode->xattr_ops = xattr_ops;
}

void robu_linux_vfs_dentry_init(robu_linux_vfs_dentry_t *dentry,
                                 robu_linux_vfs_dentry_t *parent,
                                 robu_linux_vfs_inode_t *inode,
                                 const char *name) {
    uint32_t length = 0;
    while (name[length] && length < ROBU_LINUX_VFS_NAME_MAX) {
        dentry->name[length] = name[length];
        length++;
    }
    dentry->name[length] = '\0';
    dentry->parent = parent;
    dentry->inode = inode;
    dentry->name_len = length;
}
