#include "robu/types.h"
#include "robu/uipc.h"
#include "robu/ipc.h"
#include "robu/kinfo.h"
#include "robu/testreport.h"
#include "robu/vfs.h"

#define DISKFS_TEST_PATH "/mnt/disk0/persist_test.dat"
#define DISKFS_TEST_PATTERN_LEN 24

void _start(void) {
    int checks = 0, passed = 0;
    int matched_len = 0;
    tid_t server = (tid_t)kinfo_resolve_mount(kinfo_user(), DISKFS_TEST_PATH, &matched_len);

    uint8_t pattern[DISKFS_TEST_PATTERN_LEN];
    for (int i = 0; i < DISKFS_TEST_PATTERN_LEN; i++) {
        pattern[i] = (uint8_t)(i ^ 0x7E);
    }

    checks++;
    int64_t handle = vfs_open(server, DISKFS_TEST_PATH, VFS_O_CREAT | VFS_O_TRUNC);
    if (handle >= 0) {
        passed++;

        checks++;
        int64_t wrote = vfs_write(server, (uint64_t)handle, pattern, DISKFS_TEST_PATTERN_LEN);
        if (wrote == DISKFS_TEST_PATTERN_LEN) {
            passed++;
        }

        checks++;
        if (vfs_close(server, (uint64_t)handle) == 0) {
            passed++;
        }
    } else {
        checks += 2;
    }

    msg_regs_t rep = (msg_regs_t){0};
    rep.word[0] = TEST_REPORT_KIND_DISKFS_TEST;
    rep.word[1] = (uint64_t)checks;
    rep.word[2] = (uint64_t)passed;
    rep.word[3] = (uint64_t)(checks - passed);
    rep.word[4] = 1;
    tid_t test_report_tid = (tid_t)kinfo_user()->test_report_tid;
    ipc_send(test_report_tid, &rep);

    ipc_exit(passed == checks ? 0 : 1);
}
