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
#define EXT2_RENAME_SRC "/mnt/ext2/rs"
#define EXT2_RENAME_DST "/mnt/ext2/rd"
#define EXT2_UNLINK_PATH "/mnt/ext2/ru"
#define EXT2_REMOVE_DIR "/mnt/ext2/md"
#define EXT2_REMOVE_NESTED "/mnt/ext2/md/n"
#define EXT2_MOVE_PARENT "/mnt/ext2/mp"
#define EXT2_MOVE_DIR "/mnt/ext2/mp/md"
#define EXT2_MOVE_PARENT_REF "/mnt/ext2/mp/md/.."
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

static int path_missing(tid_t server, const char *path) {
    return vfs_stat(server, path, 0, 0, 0) == VFS_ERR_NOT_FOUND ? 0 : -1;
}

static void cleanup_mutation_paths(tid_t server) {
    vfs_unlink(server, EXT2_REMOVE_NESTED);
    vfs_rmdir(server, EXT2_MOVE_DIR);
    vfs_rmdir(server, EXT2_MOVE_PARENT);
    vfs_rmdir(server, EXT2_REMOVE_DIR);
    vfs_unlink(server, EXT2_RENAME_SRC);
    vfs_unlink(server, EXT2_RENAME_DST);
    vfs_unlink(server, EXT2_UNLINK_PATH);
}

static int test_rename(tid_t server) {
    cleanup_mutation_paths(server);
    if (write_text(server, EXT2_RENAME_SRC, content, EXT2_CONTENT_LEN) != 0 ||
        vfs_rename(server, EXT2_RENAME_SRC, EXT2_RENAME_DST) != 0 ||
        path_missing(server, EXT2_RENAME_SRC) != 0 ||
        read_text(server, EXT2_RENAME_DST, content, EXT2_CONTENT_LEN) != 0) {
        cleanup_mutation_paths(server);
        return -1;
    }
    cleanup_mutation_paths(server);
    return 0;
}

static int test_rename_replace(tid_t server) {
    cleanup_mutation_paths(server);
    if (write_text(server, EXT2_RENAME_SRC, content, EXT2_CONTENT_LEN) != 0 ||
        write_text(server, EXT2_RENAME_DST, nested_content, EXT2_NESTED_LEN) != 0 ||
        vfs_rename(server, EXT2_RENAME_SRC, EXT2_RENAME_DST) != 0 ||
        path_missing(server, EXT2_RENAME_SRC) != 0 ||
        read_text(server, EXT2_RENAME_DST, content, EXT2_CONTENT_LEN) != 0) {
        cleanup_mutation_paths(server);
        return -1;
    }
    cleanup_mutation_paths(server);
    return 0;
}

static int test_unlink_open_file(tid_t server) {
    cleanup_mutation_paths(server);
    if (write_text(server, EXT2_UNLINK_PATH, content, EXT2_CONTENT_LEN) != 0) {
        return -1;
    }
    int64_t handle = vfs_open(server, EXT2_UNLINK_PATH, 0);
    if (handle < 0 || vfs_unlink(server, EXT2_UNLINK_PATH) != 0) {
        if (handle >= 0) {
            vfs_close(server, (uint64_t)handle);
        }
        cleanup_mutation_paths(server);
        return -1;
    }
    uint8_t buf[VFS_READ_MAX];
    uint64_t got = 0;
    int ok = 1;
    while (got < EXT2_CONTENT_LEN) {
        uint64_t ask = EXT2_CONTENT_LEN - got;
        if (ask > VFS_READ_MAX) {
            ask = VFS_READ_MAX;
        }
        int64_t n = vfs_read(server, (uint64_t)handle, buf, ask);
        if (n <= 0) {
            ok = 0;
            break;
        }
        for (int64_t i = 0; i < n; i++) {
            if (buf[i] != (uint8_t)content[got + (uint64_t)i]) {
                ok = 0;
            }
        }
        got += (uint64_t)n;
    }
    if (got != EXT2_CONTENT_LEN) {
        ok = 0;
    }
    int64_t close_rc = vfs_close(server, (uint64_t)handle);
    int missing = path_missing(server, EXT2_UNLINK_PATH) == 0;
    cleanup_mutation_paths(server);
    return ok && close_rc == 0 && missing ? 0 : -1;
}

static int test_rmdir(tid_t server) {
    cleanup_mutation_paths(server);
    if (vfs_mkdir(server, EXT2_REMOVE_DIR) != 0 ||
        write_text(server, EXT2_REMOVE_NESTED, nested_content, EXT2_NESTED_LEN) != 0 ||
        vfs_rmdir(server, EXT2_REMOVE_DIR) != VFS_ERR_NOT_EMPTY ||
        vfs_unlink(server, EXT2_REMOVE_NESTED) != 0 ||
        vfs_rmdir(server, EXT2_REMOVE_DIR) != 0 ||
        path_missing(server, EXT2_REMOVE_DIR) != 0) {
        cleanup_mutation_paths(server);
        return -1;
    }
    cleanup_mutation_paths(server);
    return 0;
}

static int test_rename_directory(tid_t server) {
    cleanup_mutation_paths(server);
    if (vfs_mkdir(server, EXT2_REMOVE_DIR) != 0 ||
        vfs_mkdir(server, EXT2_MOVE_PARENT) != 0 ||
        vfs_rename(server, EXT2_REMOVE_DIR, EXT2_MOVE_DIR) != 0 ||
        path_missing(server, EXT2_REMOVE_DIR) != 0) {
        cleanup_mutation_paths(server);
        return -1;
    }
    uint64_t moved_ino = 0;
    uint64_t parent_ino = 0;
    int moved_is_dir = 0;
    int parent_is_dir = 0;
    int64_t moved_rc = vfs_stat(server, EXT2_MOVE_DIR, 0, &moved_is_dir, &moved_ino);
    int64_t parent_rc = vfs_stat(server, EXT2_MOVE_PARENT, 0, &parent_is_dir, &parent_ino);
    uint64_t dotdot_ino = 0;
    int dotdot_is_dir = 0;
    int64_t dotdot_rc = vfs_stat(server, EXT2_MOVE_PARENT_REF, 0, &dotdot_is_dir, &dotdot_ino);
    int cleanup_rc = vfs_rmdir(server, EXT2_MOVE_DIR) == 0 &&
                     vfs_rmdir(server, EXT2_MOVE_PARENT) == 0;
    cleanup_mutation_paths(server);
    return moved_rc == 0 && moved_is_dir && moved_ino != 0 && parent_rc == 0 &&
           parent_is_dir && parent_ino != 0 && dotdot_rc == 0 && dotdot_is_dir &&
           dotdot_ino == parent_ino && cleanup_rc ? 0 : -1;
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
    if (test_rename(server) == 0) {
        passed++;
    } else {
        fail_mask |= 1ULL << (checks - 1);
    }

    checks++;
    if (test_rename_replace(server) == 0) {
        passed++;
    } else {
        fail_mask |= 1ULL << (checks - 1);
    }

    checks++;
    if (test_unlink_open_file(server) == 0) {
        passed++;
    } else {
        fail_mask |= 1ULL << (checks - 1);
    }

    checks++;
    if (test_rmdir(server) == 0) {
        passed++;
    } else {
        fail_mask |= 1ULL << (checks - 1);
    }

    checks++;
    if (test_rename_directory(server) == 0) {
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
