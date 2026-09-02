int ext2_mount(void);
int ext2_resolve_path(const char *path, unsigned int *out_ino, unsigned int *out_size, int *out_is_dir);
int ext2_read_file(unsigned int ino, unsigned int size, __UINTPTR_TYPE__ dst);
int ext2_read_range(unsigned int ino, unsigned int file_off, unsigned int length, __UINTPTR_TYPE__ dst);
