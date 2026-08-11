#include "robu/types.h"
#include "robu/uipc.h"
#include "robu/ipc.h"
#include "robu/signal.h"
#include "robu/testreport.h"
#include "robu/kinfo.h"

static volatile int handler_ran = 0;

static void handler(int signum) {
    handler_ran = signum;
}

void _start(void) {
    msg_regs_t m = (msg_regs_t){0};
    m.word[0] = SYS_INFO_CAT_SIGACTION;
    m.word[1] = SIGUSR1;
    m.word[2] = (uint64_t)handler;
    m.word[3] = 0;
    m.word[4] = 1;
    m.word[5] = 0;
    robu_ipc_raw(0, 0, IPC_FLAG_SYS_INFO, &m, NULL);

    msg_regs_t self = (msg_regs_t){0};
    robu_ipc_raw(0, 0, IPC_FLAG_SELF_TID, &self, NULL);
    tid_t self_tid = (tid_t)self.word[0];

    msg_regs_t k = (msg_regs_t){0};
    k.word[0] = SYS_INFO_CAT_KILL;
    k.word[1] = self_tid;
    k.word[2] = SIGUSR1;
    robu_ipc_raw(0, 0, IPC_FLAG_SYS_INFO, &k, NULL);

    int passed = (handler_ran == SIGUSR1) ? 1 : 0;

    msg_regs_t rep = (msg_regs_t){0};
    rep.word[0] = TEST_REPORT_KIND_SIGNAL_TEST;
    rep.word[1] = 1;
    rep.word[2] = (uint64_t)passed;
    rep.word[3] = passed ? 0 : 1;
    tid_t test_report_tid = (tid_t)kinfo_user()->test_report_tid;
    ipc_send(test_report_tid, &rep);

    ipc_exit(passed ? 0 : 1);
}
