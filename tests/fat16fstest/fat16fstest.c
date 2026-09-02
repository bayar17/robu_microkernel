#include "robu/types.h"
#include "robu/uipc.h"
#include "robu/ipc.h"
#include "robu/kinfo.h"
#include "robu/testreport.h"
#include "robu/vfs.h"

#define FAT16_TEST_PATH "/mnt/fat16/TESTFILE.TXT"

static const char expected[] =
    "Hello from a real FAT16 filesystem, verified via mtools.\n";
#define FAT16_TEST_LEN (sizeof(expected) - 1)

void _start(void) {
    int checks = 0, passed = 0;
    int matched_len = 0;
    tid_t server = (tid_t)kinfo_resolve_mount(kinfo_user(), FAT16_TEST_PATH, &matched_len);

    checks++;
    int64_t handle = vfs_open(server, FAT16_TEST_PATH, 0);
    if (handle >= 0) {
        passed++;

        checks++;
        uint8_t readback[FAT16_TEST_LEN];
        uint64_t got = 0;
        while (got < FAT16_TEST_LEN) {
            int64_t n = vfs_read(server, (uint64_t)handle, readback + got, FAT16_TEST_LEN - got);
            if (n <= 0) {
                break;
            }
            got += (uint64_t)n;
        }
        if (got == FAT16_TEST_LEN) {
            int match = 1;
            for (uint64_t i = 0; i < FAT16_TEST_LEN; i++) {
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

    msg_regs_t rep = (msg_regs_t){0};
    rep.word[0] = TEST_REPORT_KIND_FAT16FS_TEST;
    rep.word[1] = (uint64_t)checks;
    rep.word[2] = (uint64_t)passed;
    rep.word[3] = (uint64_t)(checks - passed);
    rep.word[4] = 1;
    tid_t test_report_tid = (tid_t)kinfo_user()->test_report_tid;
    ipc_send(test_report_tid, &rep);

    ipc_exit(passed == checks ? 0 : 1);
}
