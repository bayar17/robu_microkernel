#include "robu/types.h"
#include "robu/uipc.h"
#include "robu/ipc.h"
#include "robu/kinfo.h"
#include "robu/testreport.h"
#include "robu/kprintf.h"

#define TEST_AF_UNIX 1
#define TEST_SOCK_STREAM 1
#define SOCK_TEST_PATH "/tmp/.socktest"
#define SOCK_TEST_PATTERN_LEN 16
#define SOCK_TEST_POLL_MAX 20000

static void pack_path(uint64_t words[4], const char *path) {
    for (int i = 0; i < 4; i++) {
        words[i] = 0;
    }
    for (int i = 0; path[i] && i < 32; i++) {
        words[i / 8] |= ((uint64_t)(uint8_t)path[i]) << (8 * (i % 8));
    }
}

static int64_t sock_create_call(int domain, int type, int *out_id) {
    msg_regs_t m = (msg_regs_t){0};
    m.word[0] = SYS_INFO_CAT_SOCK_CREATE;
    m.word[1] = (uint64_t)domain;
    m.word[2] = (uint64_t)type;
    int64_t rc = robu_ipc_raw(0, 0, IPC_FLAG_SYS_INFO, &m, NULL);
    if (rc == IPC_ERR_NONE) {
        *out_id = (int)(int64_t)m.word[0];
    }
    return rc;
}
static int64_t sock_bind_call(int sockid, const char *path) {
    msg_regs_t m = (msg_regs_t){0};
    m.word[0] = SYS_INFO_CAT_SOCK_BIND;
    m.word[1] = (uint64_t)(int64_t)sockid;
    uint64_t w[4];
    pack_path(w, path);
    m.word[2] = w[0];
    m.word[3] = w[1];
    m.word[4] = w[2];
    m.word[5] = w[3];
    return robu_ipc_raw(0, 0, IPC_FLAG_SYS_INFO, &m, NULL);
}
static int64_t sock_listen_call(int sockid, int backlog) {
    msg_regs_t m = (msg_regs_t){0};
    m.word[0] = SYS_INFO_CAT_SOCK_LISTEN;
    m.word[1] = (uint64_t)(int64_t)sockid;
    m.word[2] = (uint64_t)backlog;
    return robu_ipc_raw(0, 0, IPC_FLAG_SYS_INFO, &m, NULL);
}
static int64_t sock_accept_call(int sockid, int *out_new_id) {
    msg_regs_t m = (msg_regs_t){0};
    m.word[0] = SYS_INFO_CAT_SOCK_ACCEPT;
    m.word[1] = (uint64_t)(int64_t)sockid;
    int64_t rc = robu_ipc_raw(0, 0, IPC_FLAG_SYS_INFO, &m, NULL);
    if (rc == IPC_ERR_NONE) {
        *out_new_id = (int)(int64_t)m.word[0];
    }
    return rc;
}
static int64_t sock_read_call(int sockid, uint8_t *buf, int max, int *out_n) {
    msg_regs_t m = (msg_regs_t){0};
    m.word[0] = SYS_INFO_CAT_SOCK_READ;
    m.word[1] = (uint64_t)(int64_t)sockid;
    m.word[2] = (uint64_t)max;
    int64_t rc = robu_ipc_raw(0, 0, IPC_FLAG_SYS_INFO, &m, NULL);
    if (rc == IPC_ERR_NONE) {
        int n = (int)m.word[0];
        uint64_t words[5] = { m.word[1], m.word[2], m.word[3], m.word[4], m.word[5] };
        for (int i = 0; i < n; i++) {
            buf[i] = (uint8_t)(words[i / 8] >> (8 * (i % 8)));
        }
        *out_n = n;
    }
    return rc;
}
static int64_t sock_write_call(int sockid, const uint8_t *buf, int len, int *out_n) {
    msg_regs_t m = (msg_regs_t){0};
    m.word[0] = SYS_INFO_CAT_SOCK_WRITE;
    m.word[1] = (uint64_t)(int64_t)sockid;
    int chunk = len > 24 ? 24 : len;
    m.word[2] = (uint64_t)chunk;
    uint64_t words[3] = {0, 0, 0};
    for (int i = 0; i < chunk; i++) {
        words[i / 8] |= ((uint64_t)buf[i]) << (8 * (i % 8));
    }
    m.word[3] = words[0];
    m.word[4] = words[1];
    m.word[5] = words[2];
    int64_t rc = robu_ipc_raw(0, 0, IPC_FLAG_SYS_INFO, &m, NULL);
    if (rc == IPC_ERR_NONE) {
        *out_n = (int)m.word[0];
    }
    return rc;
}
static int64_t sock_close_call(int sockid) {
    msg_regs_t m = (msg_regs_t){0};
    m.word[0] = SYS_INFO_CAT_SOCK_CLOSE;
    m.word[1] = (uint64_t)(int64_t)sockid;
    return robu_ipc_raw(0, 0, IPC_FLAG_SYS_INFO, &m, NULL);
}

void _start(void) {
    int checks = 0, passed = 0;
    int sockid = -1, connid = -1;

    checks++;
    if (sock_create_call(TEST_AF_UNIX, TEST_SOCK_STREAM, &sockid) == IPC_ERR_NONE) {
        passed++;
    }

    checks++;
    if (sock_bind_call(sockid, SOCK_TEST_PATH) == IPC_ERR_NONE) {
        passed++;
    }

    checks++;
    if (sock_listen_call(sockid, 1) == IPC_ERR_NONE) {
        passed++;
    }

    checks++;
    int accepted = 0;
    for (int i = 0; i < SOCK_TEST_POLL_MAX; i++) {
        if (sock_accept_call(sockid, &connid) == IPC_ERR_NONE) {
            accepted = 1;
            break;
        }
        ipc_sleep(1);
    }
    if (accepted) {
        passed++;
    }

    checks++;
    uint8_t rxbuf[SOCK_TEST_PATTERN_LEN] = {0};
    int total_rx = 0;
    int rx_ok = 0;
    for (int i = 0; i < SOCK_TEST_POLL_MAX && total_rx < SOCK_TEST_PATTERN_LEN; i++) {
        int n = 0;
        int64_t rc = sock_read_call(connid, rxbuf + total_rx, SOCK_TEST_PATTERN_LEN - total_rx, &n);
        if (rc == IPC_ERR_NONE) {
            total_rx += n;
        }
        if (total_rx >= SOCK_TEST_PATTERN_LEN) {
            rx_ok = 1;
            break;
        }
        ipc_sleep(1);
    }
    if (rx_ok) {
        int match = 1;
        for (int i = 0; i < SOCK_TEST_PATTERN_LEN; i++) {
            if (rxbuf[i] != (uint8_t)(i ^ 0x5A)) {
                match = 0;
                break;
            }
        }
        if (match) {
            passed++;
        }
    }

    checks++;
    uint8_t txbuf[SOCK_TEST_PATTERN_LEN];
    for (int i = 0; i < SOCK_TEST_PATTERN_LEN; i++) {
        txbuf[i] = (uint8_t)(i ^ 0xC3);
    }
    int total_tx = 0;
    int tx_ok = 0;
    while (total_tx < SOCK_TEST_PATTERN_LEN) {
        int n = 0;
        if (sock_write_call(connid, txbuf + total_tx, SOCK_TEST_PATTERN_LEN - total_tx, &n) != IPC_ERR_NONE) {
            break;
        }
        total_tx += n;
    }
    if (total_tx == SOCK_TEST_PATTERN_LEN) {
        tx_ok = 1;
    }
    if (tx_ok) {
        passed++;
    }

    checks++;
    if (sock_close_call(connid) == IPC_ERR_NONE) {
        passed++;
    }

    checks++;
    if (sock_close_call(sockid) == IPC_ERR_NONE) {
        passed++;
    }

    msg_regs_t rep = (msg_regs_t){0};
    rep.word[0] = TEST_REPORT_KIND_SOCK_TEST;
    rep.word[1] = (uint64_t)checks;
    rep.word[2] = (uint64_t)passed;
    rep.word[3] = (uint64_t)(checks - passed);
    rep.word[4] = 1;
    tid_t test_report_tid = (tid_t)kinfo_user()->test_report_tid;
    ipc_send(test_report_tid, &rep);

    ipc_exit(passed == checks ? 0 : 1);
}
