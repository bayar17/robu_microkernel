#include "robu/types.h"
#include "robu/uipc.h"
#include "robu/ipc.h"
#include "robu/vfs.h"
#include "robu/kinfo.h"
#include "robu/testreport.h"
#include "robu/kprintf.h"

void _start(void) {
    msg_regs_t m = (msg_regs_t){0};
    m.word[0] = SYS_INFO_CAT_CONSOLE_MODE;
    m.word[1] = 1;
    robu_ipc_raw(0, 0, IPC_FLAG_SYS_INFO, &m, NULL);

    tid_t devfs = (tid_t)kinfo_user()->devfs_tid;
    int64_t h = vfs_open(devfs, "console", 0);

    int checks = 0, passed = 0;
    uint8_t buf[8];

    for (int i = 0; i < 3; i++) {
        int64_t n;
        do {
            n = vfs_read(devfs, (uint64_t)h, buf, 1);
        } while (n <= 0);
        checks++;
        if (n == 1) {
            passed++;
        }
    }

    vfs_close(devfs, (uint64_t)h);

    m = (msg_regs_t){0};
    m.word[0] = SYS_INFO_CAT_CONSOLE_MODE;
    m.word[1] = 0;
    robu_ipc_raw(0, 0, IPC_FLAG_SYS_INFO, &m, NULL);

    msg_regs_t rep = (msg_regs_t){0};
    rep.word[0] = TEST_REPORT_KIND_CONSOLE_TEST;
    rep.word[1] = (uint64_t)checks;
    rep.word[2] = (uint64_t)passed;
    rep.word[3] = (uint64_t)(checks - passed);
    tid_t test_report_tid = (tid_t)kinfo_user()->test_report_tid;
    ipc_send(test_report_tid, &rep);

    ipc_exit(passed == checks ? 0 : 1);
}
