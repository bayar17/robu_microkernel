#include "robu/types.h"
#include "robu/uipc.h"
#include "robu/ipc.h"
#include "robu/kinfo.h"
#include "robu/testreport.h"
#include "robu/vfs.h"

#define EXT2_TEST_PATH "/mnt/ext2/testfile.txt"
#define EXT2_NEW_PATH "/mnt/ext2/newfile.txt"
#define EXT2_LINK_PATH "/mnt/ext2/ln"
#define EXT2_MKDIR_PATH "/mnt/ext2/mkdirtest"
#define EXT2_DIR_LINK_PATH "/mnt/ext2/ld"
#define EXT2_NESTED_PATH "/mnt/ext2/mkdirtest/nested.txt"
#define EXT2_LARGE_PATH "/mnt/ext2/large.bin"
#define EXT2_SEEDED_DIR "/mnt/ext2/bin"
#define EXT2_LARGE_LEN (300 * 1024 + 37)

static const char expected[] =
    "Hello from a real ext2 filesystem, verified via e2fsprogs.\n";
#define EXT2_TEST_LEN (sizeof(expected) - 1)

static const char content[] =
    "This file was created and written entirely from inside Robu on ext2.\n";
#define EXT2_CONTENT_LEN (sizeof(content) - 1)

static const char nested_content[] =
    "A file inside a real ext2 subdirectory created by mkdir.\n";
#define EXT2_NESTED_LEN (sizeof(nested_content) - 1)

static const char truncated_content[] = "double-indirect truncate verified\n";
#define EXT2_TRUNCATED_LEN (sizeof(truncated_content) - 1)

static uint8_t large_byte(uint64_t off) {
    return (uint8_t)((off * 29 + 17) & 0xff);
}

static int read_text(tid_t server, const char *path, const char *want, uint64_t len) {
    int64_t handle = vfs_open(server, path, 0);
    if (handle < 0) {
        return -1;
    }
    uint64_t got = 0;
    uint8_t buf[VFS_READ_MAX];
    while (got < len) {
        uint64_t ask = len - got;
        if (ask > VFS_READ_MAX) {
            ask = VFS_READ_MAX;
        }
        int64_t n = vfs_read(server, (uint64_t)handle, buf, ask);
        if (n <= 0) {
            break;
        }
        for (int64_t i = 0; i < n; i++) {
            if (buf[i] != (uint8_t)want[got + (uint64_t)i]) {
                vfs_close(server, (uint64_t)handle);
                return -1;
            }
        }
        got += (uint64_t)n;
    }
    vfs_close(server, (uint64_t)handle);
    return got == len ? 0 : -1;
}

static int write_text(tid_t server, const char *path, const char *data, uint64_t len) {
    int64_t handle = vfs_open(server, path, VFS_O_CREAT | VFS_O_TRUNC);
    if (handle < 0) {
        return -1;
    }
    uint64_t sent = 0;
    while (sent < len) {
        uint64_t ask = len - sent;
        if (ask > VFS_WRITE_MAX) {
            ask = VFS_WRITE_MAX;
        }
        int64_t n = vfs_write(server, (uint64_t)handle, data + sent, ask);
        if (n <= 0) {
            break;
        }
        sent += (uint64_t)n;
    }
    vfs_close(server, (uint64_t)handle);
    return sent == len ? 0 : -1;
}

static int fstat_path(tid_t server, const char *path, uint64_t want_size) {
    int64_t handle = vfs_open(server, path, 0);
    if (handle < 0) {
        return -1;
    }
    uint64_t size = 0;
    uint64_t ino = 0;
    int is_dir = 0;
    int64_t rc = vfs_fstat(server, (uint64_t)handle, &size, &is_dir, &ino);
    int64_t close_rc = vfs_close(server, (uint64_t)handle);
    return rc == 0 && close_rc == 0 && size == want_size && !is_dir && ino != 0 ? 0 : -1;
}

static int write_large(tid_t server) {
    int64_t handle = vfs_open(server, EXT2_LARGE_PATH, VFS_O_CREAT | VFS_O_TRUNC);
    if (handle < 0) {
        return -1;
    }
    uint64_t sent = 0;
    uint8_t buf[VFS_WRITE_MAX];
    while (sent < EXT2_LARGE_LEN) {
        uint64_t ask = EXT2_LARGE_LEN - sent;
        if (ask > VFS_WRITE_MAX) {
            ask = VFS_WRITE_MAX;
        }
        for (uint64_t i = 0; i < ask; i++) {
            buf[i] = large_byte(sent + i);
        }
        int64_t n = vfs_write(server, (uint64_t)handle, buf, ask);
        if (n <= 0) {
            break;
        }
        sent += (uint64_t)n;
    }
    vfs_close(server, (uint64_t)handle);
    return sent == EXT2_LARGE_LEN ? 0 : -1;
}

static int read_large(tid_t server) {
    int64_t handle = vfs_open(server, EXT2_LARGE_PATH, 0);
    if (handle < 0) {
        return -1;
    }
    uint64_t got = 0;
    uint8_t buf[VFS_READ_MAX];
    while (got < EXT2_LARGE_LEN) {
        uint64_t ask = EXT2_LARGE_LEN - got;
        if (ask > VFS_READ_MAX) {
            ask = VFS_READ_MAX;
        }
        int64_t n = vfs_read(server, (uint64_t)handle, buf, ask);
        if (n <= 0) {
            break;
        }
        for (int64_t i = 0; i < n; i++) {
            if (buf[i] != large_byte(got + (uint64_t)i)) {
                vfs_close(server, (uint64_t)handle);
                return -1;
            }
        }
        got += (uint64_t)n;
    }
    vfs_close(server, (uint64_t)handle);
    return got == EXT2_LARGE_LEN ? 0 : -1;
}

static void report_result(int checks, int passed, uint64_t fail_mask, tid_t server) {
    msg_regs_t rep = (msg_regs_t){0};
    rep.word[0] = TEST_REPORT_KIND_EXT2FS_TEST;
    rep.word[1] = (uint64_t)checks;
    rep.word[2] = (uint64_t)passed;
    rep.word[3] = fail_mask;
    rep.word[4] = server;
    tid_t test_report_tid = (tid_t)kinfo_user()->test_report_tid;
    ipc_send(test_report_tid, &rep);
    ipc_exit(passed == checks ? 0 : 1);
}

void _start(void) {
    int checks = 0;
    int passed = 0;
    uint64_t fail_mask = 0;
    int matched_len = 0;
    tid_t server = (tid_t)kinfo_resolve_mount(kinfo_user(), EXT2_TEST_PATH, &matched_len);

    checks++;
    if (read_text(server, EXT2_TEST_PATH, expected, EXT2_TEST_LEN) == 0) {
        passed++;
    } else {
        fail_mask |= 1ULL << (checks - 1);
    }

    checks++;
    uint64_t seeded_size = 0;
    int seeded_is_dir = 0;
    if (vfs_stat(server, EXT2_SEEDED_DIR, &seeded_size, &seeded_is_dir, 0) == 0 && seeded_is_dir) {
        passed++;
    } else {
        fail_mask |= 1ULL << (checks - 1);
    }

    uint64_t existing_size = 0;
    int existing_is_dir = 0;
    int verify = vfs_stat(server, EXT2_NEW_PATH, &existing_size, &existing_is_dir, 0) == 0;

    if (!verify) {
        checks++;
        if (write_text(server, EXT2_NEW_PATH, content, EXT2_CONTENT_LEN) == 0) {
            passed++;
        } else {
            fail_mask |= 1ULL << (checks - 1);
        }

        checks++;
        if (read_text(server, EXT2_NEW_PATH, content, EXT2_CONTENT_LEN) == 0) {
            passed++;
        } else {
            fail_mask |= 1ULL << (checks - 1);
        }

        checks++;
        if (vfs_symlink(server, EXT2_LINK_PATH, "newfile.txt") == 0) {
            passed++;
        } else {
            fail_mask |= 1ULL << (checks - 1);
        }

        checks++;
        if (read_text(server, EXT2_LINK_PATH, content, EXT2_CONTENT_LEN) == 0) {
            passed++;
        } else {
            fail_mask |= 1ULL << (checks - 1);
        }

        checks++;
        if (vfs_mkdir(server, EXT2_MKDIR_PATH) == 0) {
            passed++;
        } else {
            fail_mask |= 1ULL << (checks - 1);
        }

        checks++;
        uint64_t mkdir_size = 0;
        int mkdir_is_dir = 0;
        if (vfs_stat(server, EXT2_MKDIR_PATH, &mkdir_size, &mkdir_is_dir, 0) == 0 && mkdir_is_dir) {
            passed++;
        } else {
            fail_mask |= 1ULL << (checks - 1);
        }

        checks++;
        if (write_text(server, EXT2_NESTED_PATH, nested_content, EXT2_NESTED_LEN) == 0) {
            passed++;
        } else {
            fail_mask |= 1ULL << (checks - 1);
        }

        checks++;
        if (read_text(server, EXT2_NESTED_PATH, nested_content, EXT2_NESTED_LEN) == 0) {
            passed++;
        } else {
            fail_mask |= 1ULL << (checks - 1);
        }

        checks++;
        if (vfs_symlink(server, EXT2_DIR_LINK_PATH, "mkdirtest") == 0) {
            passed++;
        } else {
            fail_mask |= 1ULL << (checks - 1);
        }

        checks++;
        if (read_text(server, "/mnt/ext2/ld/nested.txt", nested_content, EXT2_NESTED_LEN) == 0) {
            passed++;
        } else {
            fail_mask |= 1ULL << (checks - 1);
        }

        checks++;
        if (write_large(server) == 0) {
            passed++;
        } else {
            fail_mask |= 1ULL << (checks - 1);
        }

        checks++;
        if (read_large(server) == 0) {
            passed++;
        } else {
            fail_mask |= 1ULL << (checks - 1);
        }

        checks++;
        if (write_text(server, EXT2_LARGE_PATH, truncated_content, EXT2_TRUNCATED_LEN) == 0) {
            passed++;
        } else {
            fail_mask |= 1ULL << (checks - 1);
        }
    }

    checks++;
    if (read_text(server, EXT2_NEW_PATH, content, EXT2_CONTENT_LEN) == 0) {
        passed++;
    } else {
        fail_mask |= 1ULL << (checks - 1);
    }

    checks++;
    if (read_text(server, EXT2_LINK_PATH, content, EXT2_CONTENT_LEN) == 0) {
        passed++;
    } else {
        fail_mask |= 1ULL << (checks - 1);
    }

    checks++;
    if (fstat_path(server, EXT2_NEW_PATH, EXT2_CONTENT_LEN) == 0) {
        passed++;
    } else {
        fail_mask |= 1ULL << (checks - 1);
    }

    checks++;
    uint64_t mkdir_size = 0;
    int mkdir_is_dir = 0;
    if (vfs_stat(server, EXT2_MKDIR_PATH, &mkdir_size, &mkdir_is_dir, 0) == 0 && mkdir_is_dir) {
        passed++;
    } else {
        fail_mask |= 1ULL << (checks - 1);
    }

    checks++;
    if (read_text(server, EXT2_NESTED_PATH, nested_content, EXT2_NESTED_LEN) == 0) {
        passed++;
    } else {
        fail_mask |= 1ULL << (checks - 1);
    }

    checks++;
    if (read_text(server, "/mnt/ext2/ld/nested.txt", nested_content, EXT2_NESTED_LEN) == 0) {
        passed++;
    } else {
        fail_mask |= 1ULL << (checks - 1);
    }

    checks++;
    if (read_text(server, EXT2_LARGE_PATH, truncated_content, EXT2_TRUNCATED_LEN) == 0) {
        passed++;
    } else {
        fail_mask |= 1ULL << (checks - 1);
    }

    checks++;
    if (vfs_quiesce(server) == 0) {
        passed++;
    } else {
        fail_mask |= 1ULL << (checks - 1);
    }

    report_result(checks, passed, fail_mask, server);
}
