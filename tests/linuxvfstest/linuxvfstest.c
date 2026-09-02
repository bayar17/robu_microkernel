#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/random.h>
#include <sys/xattr.h>
#include <time.h>
#include <unistd.h>

#include <robu/vfs.h>

static int checks;
static int passed;

static void check(int ok) {
    checks++;
    if (ok) {
        passed++;
    }
}

static int bytes_equal(const char *left, const char *right, size_t len) {
    for (size_t i = 0; i < len; i++) {
        if (left[i] != right[i]) {
            return 0;
        }
    }
    return 1;
}

static int xattr_list_contains(const char *list, ssize_t list_len, const char *name) {
    size_t name_len = 0;
    while (name[name_len]) {
        name_len++;
    }
    ssize_t offset = 0;
    while (offset < list_len) {
        size_t entry_len = 0;
        while (offset + (ssize_t)entry_len < list_len && list[offset + (ssize_t)entry_len]) {
            entry_len++;
        }
        if (offset + (ssize_t)entry_len >= list_len) {
            return 0;
        }
        if (entry_len == name_len && bytes_equal(list + offset, name, name_len)) {
            return 1;
        }
        offset += (ssize_t)entry_len + 1;
    }
    return 0;
}

int main(void) {
    uint8_t entropy[64];
    uint8_t random_bytes[64];
    const char value[] = "robu";
    const char replacement[] = "kernel";
    const char second[] = "ext2";
    const char *path = "/linuxvfstest.tmp";
    uint64_t abi = 0;
    uint64_t features = 0;
    int entropy_ready;
    check(robu_vfs_root_capabilities(&abi, &features) == 0);
    check((uint32_t)(abi >> 32) == ROBU_VFS_ABI_MAJOR &&
          (features & (ROBU_VFS_FEATURE_OPEN | ROBU_VFS_FEATURE_READ |
                       ROBU_VFS_FEATURE_WRITE | ROBU_VFS_FEATURE_LINUX_VFS)) ==
          (ROBU_VFS_FEATURE_OPEN | ROBU_VFS_FEATURE_READ |
           ROBU_VFS_FEATURE_WRITE | ROBU_VFS_FEATURE_LINUX_VFS));
    check((features & ROBU_VFS_FEATURE_XATTR) != 0);
    check((features & ROBU_VFS_FEATURE_TIMESTAMPS) != 0);

    errno = 0;
    entropy_ready = getentropy(entropy, sizeof(entropy)) == 0;
    check(entropy_ready || errno == ENOSYS);
    check(getentropy(entropy, 257) == -1 && errno == EINVAL);
    errno = 0;
    ssize_t random_result = getrandom(random_bytes, sizeof(random_bytes), 0);
    check((entropy_ready && random_result == (ssize_t)sizeof(random_bytes)) ||
          (!entropy_ready && random_result == -1 && errno == ENOSYS));
    errno = 0;
    check(getrandom(random_bytes, sizeof(random_bytes), 0x80000000U) == -1 && errno == EINVAL);

    struct stat prior;
    errno = 0;
    int prior_result = stat(path, &prior);
    check((prior_result == -1 && errno == ENOENT) ||
          (prior_result == 0 && prior.st_atim.tv_sec == 123456 &&
           prior.st_mtim.tv_sec == 234567));

    int fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0644);
    check(fd >= 0);
    if (fd >= 0) {
        errno = 0;
        check(fsetxattr(fd, "user.robu", value, sizeof(value) - 1, XATTR_CREATE) == 0);
        errno = 0;
        check(fsetxattr(fd, "user.robu", value, sizeof(value) - 1, XATTR_CREATE) == -1 &&
              errno == EEXIST);
        check(fgetxattr(fd, "user.robu", NULL, 0) == (ssize_t)(sizeof(value) - 1));
        char got[16] = {0};
        check(fgetxattr(fd, "user.robu", got, sizeof(got)) == (ssize_t)(sizeof(value) - 1) &&
              bytes_equal(got, value, sizeof(value) - 1));
        check(fsetxattr(fd, "user.robu", replacement, sizeof(replacement) - 1, XATTR_REPLACE) == 0);
        check(fsetxattr(fd, "user.second", second, sizeof(second) - 1, 0) == 0);
        char attrs[64] = {0};
        ssize_t attrs_len = flistxattr(fd, attrs, sizeof(attrs));
        check(attrs_len > 0 && xattr_list_contains(attrs, attrs_len, "user.robu") &&
              xattr_list_contains(attrs, attrs_len, "user.second"));
        close(fd);

        got[0] = '\0';
        check(getxattr(path, "user.robu", got, sizeof(got)) == (ssize_t)(sizeof(replacement) - 1) &&
              bytes_equal(got, replacement, sizeof(replacement) - 1));
        check(removexattr(path, "user.second") == 0);
        check(removexattr(path, "user.robu") == 0);
        errno = 0;
        check(getxattr(path, "user.robu", got, sizeof(got)) == -1 && errno == ENODATA);
        struct timespec times[2];
        times[0].tv_sec = 123456;
        times[0].tv_nsec = 0;
        times[1].tv_sec = 234567;
        times[1].tv_nsec = 0;
        check(utimensat(AT_FDCWD, path, times, 0) == 0);
        struct stat st;
        check(stat(path, &st) == 0 && st.st_atim.tv_sec == times[0].tv_sec &&
              st.st_mtim.tv_sec == times[1].tv_sec);
    } else {
        for (int i = 0; i < 13; i++) {
            check(0);
        }
    }

    printf("[linux-vfs-test] %d/%d checks passed (ext2 xattrs and timestamps persistent, entropy=%s)\n",
           passed, checks, entropy_ready ? "rdrand" : "unavailable");
    return passed == checks ? 0 : 1;
}
