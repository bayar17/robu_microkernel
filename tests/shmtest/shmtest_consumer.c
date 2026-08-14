#include "robu/types.h"
#include "robu/uipc.h"
#include "robu/ipc.h"
#include "robu/kinfo.h"
#include "robu/testreport.h"
#include "robu/kprintf.h"

#define SHM_TEST_KEY   0x53484D31
#define SHM_TEST_PATTERN_LEN 64
#define SHM_TEST_READY_OFFSET 4088
#define SHM_TEST_READY_MAGIC  0x1122334455667788ULL
#define SHM_TEST_DONE_OFFSET  4080
#define SHM_TEST_DONE_MAGIC   0x99AABBCCDDEEFF00ULL
#define SHM_TEST_POLL_MAX 20000

static int64_t shm_get_call(int key, uint64_t size, uint32_t shmflg, int *out_id) {
    msg_regs_t m = (msg_regs_t){0};
    m.word[0] = SYS_INFO_CAT_SHM_GET;
    m.word[1] = (uint64_t)(int64_t)key;
    m.word[2] = size;
    m.word[3] = shmflg;
    int64_t rc = robu_ipc_raw(0, 0, IPC_FLAG_SYS_INFO, &m, NULL);
    if (rc == IPC_ERR_NONE) {
        *out_id = (int)(int64_t)m.word[0];
    }
    return rc;
}
static int64_t shm_at_call(int shmid, uint32_t shmflg, uint64_t *out_va) {
    msg_regs_t m = (msg_regs_t){0};
    m.word[0] = SYS_INFO_CAT_SHM_AT;
    m.word[1] = (uint64_t)(int64_t)shmid;
    m.word[2] = 0;
    m.word[3] = shmflg;
    int64_t rc = robu_ipc_raw(0, 0, IPC_FLAG_SYS_INFO, &m, NULL);
    if (rc == IPC_ERR_NONE) {
        *out_va = m.word[0];
    }
    return rc;
}
static int64_t shm_dt_call(uint64_t shmaddr) {
    msg_regs_t m = (msg_regs_t){0};
    m.word[0] = SYS_INFO_CAT_SHM_DT;
    m.word[1] = shmaddr;
    return robu_ipc_raw(0, 0, IPC_FLAG_SYS_INFO, &m, NULL);
}

void _start(void) {
    int checks = 0, passed = 0;
    int shmid = -1;

    checks++;
    int found = 0;
    for (int i = 0; i < SHM_TEST_POLL_MAX; i++) {
        if (shm_get_call(SHM_TEST_KEY, 0, 0, &shmid) == IPC_ERR_NONE) {
            found = 1;
            break;
        }
        ipc_sleep(1);
    }
    if (found) {
        passed++;
    }

    uint64_t va = 0;
    checks++;
    if (shm_at_call(shmid, 0, &va) == IPC_ERR_NONE) {
        passed++;
    }

    volatile uint8_t *buf = (volatile uint8_t *)va;
    volatile uint64_t *ready = (volatile uint64_t *)(buf + SHM_TEST_READY_OFFSET);

    checks++;
    int saw_ready = 0;
    for (int i = 0; i < SHM_TEST_POLL_MAX; i++) {
        if (*ready == SHM_TEST_READY_MAGIC) {
            saw_ready = 1;
            break;
        }
        ipc_sleep(1);
    }
    if (saw_ready) {
        passed++;
    }

    checks++;
    int match = 1;
    for (int i = 0; i < SHM_TEST_PATTERN_LEN; i++) {
        if (buf[i] != (uint8_t)(i ^ 0xA5)) {
            match = 0;
            break;
        }
    }
    if (match) {
        passed++;
    }

    volatile uint64_t *done = (volatile uint64_t *)(buf + SHM_TEST_DONE_OFFSET);
    *done = SHM_TEST_DONE_MAGIC;

    checks++;
    if (shm_dt_call(va) == IPC_ERR_NONE) {
        passed++;
    }

    msg_regs_t rep = (msg_regs_t){0};
    rep.word[0] = TEST_REPORT_KIND_SHM_TEST;
    rep.word[1] = (uint64_t)checks;
    rep.word[2] = (uint64_t)passed;
    rep.word[3] = (uint64_t)(checks - passed);
    rep.word[4] = 2;
    tid_t test_report_tid = (tid_t)kinfo_user()->test_report_tid;
    ipc_send(test_report_tid, &rep);

    ipc_exit(passed == checks ? 0 : 1);
}
