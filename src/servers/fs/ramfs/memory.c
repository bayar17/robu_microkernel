
static vaddr_t heap_bump = 0x40000000ULL;

static void *ram_alloc(size_t sz) {
    sz = (sz + 7) & ~7ULL;
    void *ptr = (void*)heap_bump;
    heap_bump += sz;
    return ptr;
}

static ramfs_file_t *files = NULL;
static uint32_t max_files = 0;

static ramfs_handle_t *handles = NULL;
static uint32_t max_handles = 0;

static int name_eq(const char *a, const char *b) {
    for (int i = 0; i < VFS_PATH_MAX; i++) {
        if (a[i] != b[i]) return 0;
        if (a[i] == '\0') return 1;
    }
    return 1;
}

static void set_name(char *dst, const char *src, size_t dst_size) {
    size_t i = 0;
    for (; i < dst_size - 1 && src[i]; i++) dst[i] = src[i];
    dst[i] = '\0';
}

static int find_file(const char *name) {
    for (uint32_t i = 0; i < max_files; i++) {
        if (files[i].in_use && name_eq(files[i].name, name)) return i;
    }
    return -1;
}

static int resolve_path(const char *name) {
    char cur[VFS_PATH_MAX];
    set_name(cur, name, sizeof(cur));
    for (int hop = 0; hop < 8; hop++) {
        int idx = find_file(cur);
        if (idx < 0) return -1;
        if (!files[idx].is_symlink) return idx;
        set_name(cur, files[idx].target, sizeof(cur));
    }
    return -1;
}

static int alloc_file(void) {
    for (uint32_t i = 0; i < max_files; i++) {
        if (!files[i].in_use) return i;
    }
    uint32_t new_max = max_files == 0 ? 64 : max_files * 2;
    ramfs_file_t *new_files = (ramfs_file_t*)ram_alloc(new_max * sizeof(ramfs_file_t));
    if (files) {
        for (uint32_t i = 0; i < max_files; i++) {
            volatile uint8_t *dst = (volatile uint8_t *)&new_files[i];
            const volatile uint8_t *src = (const volatile uint8_t *)&files[i];
            for (uint64_t j = 0; j < sizeof(ramfs_file_t); j++) {
                dst[j] = src[j];
            }
        }
    }
    for (uint32_t i = max_files; i < new_max; i++) new_files[i].in_use = 0;

    int ret_idx = max_files;
    files = new_files;
    max_files = new_max;
    return ret_idx;
}

static int alloc_handle(void) {
    for (uint32_t i = 0; i < max_handles; i++) {
        if (!handles[i].in_use) return i;
    }
    uint32_t new_max = max_handles == 0 ? 32 : max_handles * 2;
    ramfs_handle_t *new_hdls = (ramfs_handle_t*)ram_alloc(new_max * sizeof(ramfs_handle_t));
    if (handles) {
        for (uint32_t i = 0; i < max_handles; i++) new_hdls[i] = handles[i];
    }
    for (uint32_t i = max_handles; i < new_max; i++) new_hdls[i].in_use = 0;

    int ret_idx = max_handles;
    handles = new_hdls;
    max_handles = new_max;
    return ret_idx;
}

static int valid_handle(uint64_t h) {
    return h < max_handles && handles[h].in_use;
}

static void xattrs_reset(ramfs_file_t *file) {
    for (int i = 0; i < RAMFS_XATTR_SLOTS; i++) {
        file->xattrs[i].in_use = 0;
    }
}

static void parent_path(const char *path, char *out, size_t out_size) {
    size_t len = 0;
    while (path[len]) len++;
    size_t last_slash = 0;
    int found = 0;
    for (size_t i = 0; i < len; i++) {
        if (path[i] == '/') { last_slash = i; found = 1; }
    }
    if (!found) { out[0] = '\0'; return; }
    size_t n = last_slash < out_size - 1 ? last_slash : out_size - 1;
    for (size_t i = 0; i < n; i++) out[i] = path[i];
    out[n] = '\0';
}

static uint64_t resolve_parent_ino(const char *path) {
    char parent[VFS_PATH_MAX];
    parent_path(path, parent, sizeof(parent));
    if (parent[0] == '\0') return VFS_ROOT_INO;
    int idx = find_file(parent);
    if (idx < 0) return VFS_ROOT_INO;
    return (uint64_t)idx + 2;
}

static int resolve_parent_checked(const char *path, uint64_t *ino_out) {
    char parent[VFS_PATH_MAX];
    parent_path(path, parent, sizeof(parent));
    if (parent[0] == '\0') { *ino_out = VFS_ROOT_INO; return 1; }
    int idx = find_file(parent);
    if (idx < 0 || !files[idx].is_dir) return 0;
    *ino_out = (uint64_t)idx + 2;
    return 1;
}

static const char *basename_of(const char *path) {
    size_t len = 0;
    while (path[len]) len++;
    size_t last_slash = 0;
    int found = 0;
    for (size_t i = 0; i < len; i++) {
        if (path[i] == '/') { last_slash = i; found = 1; }
    }
    return found ? path + last_slash + 1 : path;
}
