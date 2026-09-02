int fat32_mount(void);
int fat32_resolve_root_file(const char *name83, unsigned int *out_cluster, unsigned int *out_size);
int fat32_read_file(unsigned int cluster, unsigned int size, unsigned int dst);
