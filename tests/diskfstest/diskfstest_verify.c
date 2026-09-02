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

    checks++;
    int64_t handle = vfs_open(server, DISKFS_TEST_PATH, 0);
    if (handle >= 0) {
        passed++;

        checks++;
        uint8_t readback[DISKFS_TEST_PATTERN_LEN] = {0};
        int64_t n = vfs_read(server, (uint64_t)handle, readback, DISKFS_TEST_PATTERN_LEN);
        if (n == DISKFS_TEST_PATTERN_LEN) {
            int match = 1;
            for (int i = 0; i < DISKFS_TEST_PATTERN_LEN; i++) {
                if (readback[i] != (uint8_t)(i ^ 0x7E)) {
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
    rep.word[0] = TEST_REPORT_KIND_DISKFS_TEST;
    rep.word[1] = (uint64_t)checks;
    rep.word[2] = (uint64_t)passed;
    rep.word[3] = (uint64_t)(checks - passed);
    rep.word[4] = 2;
    tid_t test_report_tid = (tid_t)kinfo_user()->test_report_tid;
    ipc_send(test_report_tid, &rep);

    ipc_exit(passed == checks ? 0 : 1);
}
