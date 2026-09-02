#include "robu/types.h"
#include "robu/uipc.h"
#include "robu/ipc.h"
#include "robu/kinfo.h"
#include "robu/testreport.h"
#include "robu/vfs.h"

#define FAT32_TEST_PATH "/mnt/fat32/TESTFIL2.TXT"
#define FAT32_NEW_PATH  "/mnt/fat32/NEWFILE.TXT"

static const char expected[] =
    "Hello from a real FAT32 filesystem, verified via mtools.\n";
#define FAT32_TEST_LEN (sizeof(expected) - 1)

static const char content[] =
    "This file was created and written entirely from inside Robu on FAT32.\n";
#define FAT32_CONTENT_LEN (sizeof(content) - 1)

void _start(void) {
    int checks = 0, passed = 0;
    int matched_len = 0;
    tid_t server = (tid_t)kinfo_resolve_mount(kinfo_user(), FAT32_TEST_PATH, &matched_len);

    checks++;
    int64_t handle = vfs_open(server, FAT32_TEST_PATH, 0);
    if (handle >= 0) {
        passed++;

        checks++;
        uint8_t readback[FAT32_TEST_LEN];
        uint64_t got = 0;
        while (got < FAT32_TEST_LEN) {
            int64_t n = vfs_read(server, (uint64_t)handle, readback + got, FAT32_TEST_LEN - got);
            if (n <= 0) {
                break;
            }
            got += (uint64_t)n;
        }
        if (got == FAT32_TEST_LEN) {
            int match = 1;
            for (uint64_t i = 0; i < FAT32_TEST_LEN; i++) {
                if (readback[i] != (uint8_t)expected[i]) {
                    match = 0;
                    break;
                }
            }
            if (match) {
                passed++;
            }
        }
        vfs_close(server, (uint64_t)handle);
    } else {
        checks++;
    }

    checks++;
    int64_t wh = vfs_open(server, FAT32_NEW_PATH, VFS_O_CREAT | VFS_O_TRUNC);
    if (wh >= 0) {
        passed++;

        checks++;
        uint64_t sent = 0;
        while (sent < FAT32_CONTENT_LEN) {
            int64_t n = vfs_write(server, (uint64_t)wh, content + sent, FAT32_CONTENT_LEN - sent);
            if (n <= 0) {
                break;
            }
            sent += (uint64_t)n;
        }
        if (sent == FAT32_CONTENT_LEN) {
            passed++;
        }
        vfs_close(server, (uint64_t)wh);

        checks++;
        int64_t rh = vfs_open(server, FAT32_NEW_PATH, 0);
        if (rh >= 0) {
            uint8_t readback[FAT32_CONTENT_LEN];
            uint64_t got = 0;
            while (got < FAT32_CONTENT_LEN) {
                int64_t n = vfs_read(server, (uint64_t)rh, readback + got, FAT32_CONTENT_LEN - got);
                if (n <= 0) {
                    break;
                }
                got += (uint64_t)n;
            }
            if (got == FAT32_CONTENT_LEN) {
                int match = 1;
                for (uint64_t i = 0; i < FAT32_CONTENT_LEN; i++) {
                    if (readback[i] != (uint8_t)content[i]) {
                        match = 0;
                        break;
                    }
                }
                if (match) {
                    passed++;
                }
            }
            vfs_close(server, (uint64_t)rh);
        }
    } else {
        checks += 2;
    }

    msg_regs_t rep = (msg_regs_t){0};
    rep.word[0] = TEST_REPORT_KIND_FAT32FS_TEST;
    rep.word[1] = (uint64_t)checks;
    rep.word[2] = (uint64_t)passed;
    rep.word[3] = (uint64_t)(checks - passed);
    tid_t test_report_tid = (tid_t)kinfo_user()->test_report_tid;
    ipc_send(test_report_tid, &rep);

    ipc_exit(passed == checks ? 0 : 1);
}
