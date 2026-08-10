#include "robu/types.h"
#include "robu/arch.h"
#include "robu/kprintf.h"
#include "robu/sched.h"
#include "robu/elf.h"
#include "robu/cmdline.h"
#include "robu/captable.h"
#include "robu/uipc.h"
#include "robu/kinfo.h"
#include "robu/testreport.h"
#include "robu/rootfs.h"
#include "../boot.h"

static tid_t g_sh_tid;

static void test_report_entry(void) {
    msg_regs_t m;
    tid_t from;
    ramfs_bin_seed_init();
    ramfs_etc_seed_init();
    ramfs_usr_seed_init();
    ramfs_sbin_seed_init();
    const char *starter = cmdline_get("starter");
    if (!starter) {
        starter = "sh";
    }
    const char *starter_name = starter;
    for (const char *p = starter; *p; p++) {
        if (*p == '/') {
            starter_name = p + 1;
        }
    }
    static char init_cmd_buf[128];
    const char *argv[8];
    int argc = 1;
    argv[0] = starter_name;
    const char *init_cmd = cmdline_get("init_cmd");
    if (init_cmd) {
        size_t len = strlen(init_cmd);
        if (len >= sizeof(init_cmd_buf)) {
            len = sizeof(init_cmd_buf) - 1;
        }
        memcpy(init_cmd_buf, init_cmd, len);
        init_cmd_buf[len] = '\0';
        char *p = init_cmd_buf;
        while (*p && argc < 8) {
            argv[argc++] = p;
            while (*p && *p != ',') {
                p++;
            }
            if (*p == ',') {
                *p++ = '\0';
            }
        }
    }
    tcb_t *sh_task = toybox_spawn(starter_name, argc, argv, 9);
    g_sh_tid = sh_task->tid;
    toybox_pipeline_init();
    for (;;) {
        ipc_recv(IPC_TID_ANY, &m, &from);
        switch (m.word[0]) {
        case TEST_REPORT_KIND_BENCH:
            kprintf("[bench] ipc-fast-path avg_cycles=%lu iterations=%lu (tid=%u)\n",
                    m.word[1], m.word[2], from);
            break;
        case TEST_REPORT_KIND_NOTIF_LATENCY:
            SAFE_PRINT("[bench] cross-core notify latency round=%lu elapsed_cycles=%lu "
                    "(trend only, no committed budget)\n", m.word[2], m.word[1]);
            break;
        case TEST_REPORT_KIND_GRANT_THROUGHPUT:
            SAFE_PRINT("[bench] map/grant throughput avg_cycles=%lu iterations=%lu "
                    "(trend only, no committed budget)\n", m.word[1], m.word[2]);
            break;
        case TEST_REPORT_KIND_DESTROY:
            SAFE_PRINT("[fault-injection] root-task destroy: rc=%ld redestroy_rc=%ld "
                    "(expect 0, then %d)\n",
                    (int64_t)m.word[1], (int64_t)m.word[2], IPC_ERR_NO_CAP);
            break;
        case TEST_REPORT_KIND_ARGVTEST: {
            char argv0[9] = {0};
            for (int i = 0; i < 8; i++) {
                uint8_t c = (uint8_t)(m.word[4] >> (8 * i));
                if (!c) {
                    break;
                }
                argv0[i] = (char)c;
            }
            kprintf("[argv-test] argc=%lu heap_base=0x%lx envp=0x%lx argv[0][0:8]='%s'\n",
                    m.word[1], m.word[2], m.word[3], argv0);
            break;
        }
        case TEST_REPORT_KIND_EXIT: {
            char name[17] = {0};
            for (int i = 0; i < 8; i++) {
                uint8_t c = (uint8_t)(m.word[2] >> (8 * i));
                if (!c) break;
                name[i] = (char)c;
            }
            int nlen = (int)strlen(name);
            for (int i = 0; i < 8 && nlen < 16; i++) {
                uint8_t c = (uint8_t)(m.word[3] >> (8 * i));
                if (!c) break;
                name[nlen++] = (char)c;
            }
            toybox_pipeline_advance();
            if (g_sh_tid && from == g_sh_tid) {
                int status = (int)(int64_t)m.word[1];
                kprintf("[boot] %s exited status=%d\n", name, status);
                arch_test_exit(status);
            }
            break;
        }
        case TEST_REPORT_KIND_LIBC_FDTEST:
            kprintf("[libc-fdtest] %lu/%lu checks passed (fail bitmask=0x%lx)\n",
                    m.word[2], m.word[1], m.word[3]);
            break;
        case TEST_REPORT_KIND_RAMFS_TEST:
            kprintf("[ramfs-test] %lu/%lu checks passed (fail bitmask=0x%lx)\n",
                    m.word[2], m.word[1], m.word[3]);
            break;
        case TEST_REPORT_KIND_SPAWN_TEST:
            kprintf("[spawn-test] %lu/%lu checks passed (fail bitmask=0x%lx)\n",
                    m.word[2], m.word[1], m.word[3]);
            break;
        case TEST_REPORT_KIND_SIGNAL_TEST:
            kprintf("[signal-test] %lu/%lu checks passed (fail bitmask=0x%lx)\n",
                    m.word[2], m.word[1], m.word[3]);
            break;
        case TEST_REPORT_KIND_CONSOLE_TEST:
            kprintf("[console-test] %lu/%lu checks passed (fail bitmask=0x%lx)\n",
                    m.word[2], m.word[1], m.word[3]);
            break;
        default:
            kprintf("[test-report] unknown report kind=%lu from tid=%u\n", m.word[0], from);
            break;
        }
    }
}

static uint8_t stack_test_report[STACK_SIZE] __attribute__((aligned(16)));

tid_t test_report_init(void) {
    tcb_t *t = thread_create("test-report", test_report_entry, stack_test_report + STACK_SIZE, 14);
    kinfo_set_test_report_tid(t->tid);
    return t->tid;
}

static uint8_t stack_abitest_helper[STACK_SIZE] __attribute__((aligned(16)));
static void abitest_helper_entry(void) {
    for (;;) {
        ipc_sleep(1000000);
    }
}

static uint8_t stack_abitest_exit_helper[STACK_SIZE] __attribute__((aligned(16)));
static void abitest_exit_helper_entry(void) {
    for (;;) {
        msg_regs_t m;
        tid_t from;
        ipc_recv(IPC_TID_ANY, &m, &from);
        msg_regs_t probe = (msg_regs_t){0};
        int64_t rc = robu_ipc_raw(0, 0, IPC_FLAG_EXIT, &probe, NULL);
        m.word[0] = (uint64_t)rc;
        ipc_send(from, &m);
    }
}

void abitest_init(paddr_t untyped_base, uint64_t untyped_size) {
    if (!cmdline_get("abitest")) {
        return;
    }
    const uint8_t *elf_start, *elf_end;
    if (rootfs_lookup("abitest", &elf_start, &elf_end) != 0) {
        kprintf("[boot] FATAL: rootfs has no entry named 'abitest'\n");
        for (;;) { asm volatile("cli; hlt"); }
    }
    tcb_t *helper = thread_create("abitest-helper", abitest_helper_entry,
                                  stack_abitest_helper + STACK_SIZE, 5);
    kinfo_set_abitest_helper_tid(helper->tid);
    tcb_t *exit_helper = thread_create("abitest-exit-helper", abitest_exit_helper_entry,
                                       stack_abitest_exit_helper + STACK_SIZE, 5);
    kinfo_set_abitest_exit_helper_tid(exit_helper->tid);
    tcb_t *abitest = elf_load_and_spawn("abitest", elf_start, elf_end, 14, PAGER_TID);
    if (!abitest) {
        kprintf("[boot] FATAL: abitest failed to load\n");
        for (;;) { asm volatile("cli; hlt"); }
    }
    uint64_t untyped_slot = kcap_next_slot();
    kcap_grant(abitest->tid, CAP_KIND_UNTYPED, (uint64_t)untyped_base, untyped_size);
    uint64_t rt_slot, rt_addr;
    cap_retype(abitest->tid, (uint32_t)untyped_slot, CAP_KIND_NOTIFICATION,
              0, 0, 0, 0, &rt_slot, &rt_addr);
    uint64_t notif_slot = rt_slot;
    cap_retype(abitest->tid, (uint32_t)untyped_slot, CAP_KIND_TIMER,
              0, 0, 0, 0, &rt_slot, &rt_addr);
    uint64_t timer_slot = rt_slot;
    cap_retype(abitest->tid, (uint32_t)untyped_slot, CAP_KIND_FRAME,
              0, 0, 0, 0, &rt_slot, &rt_addr);
    uint64_t revoke_frame_slot = rt_slot;
    uint64_t helper_tcb_slot = kcap_next_slot();
    kcap_grant(abitest->tid, CAP_KIND_TCB, helper->tid, 0);
    kinfo_set_abitest_slots((uint32_t)untyped_slot, (uint32_t)notif_slot,
                            (uint32_t)timer_slot, (uint32_t)helper_tcb_slot,
                            (uint32_t)revoke_frame_slot);
    kprintf("[boot] abitest: tid=%u, helper tid=%u\n", abitest->tid, helper->tid);
}

void argvtest_init(void) {
    if (!cmdline_get("argvtest")) {
        return;
    }
    const uint8_t *elf_start, *elf_end;
    if (rootfs_lookup("argvtest", &elf_start, &elf_end) != 0) {
        kprintf("[boot] FATAL: rootfs has no entry named 'argvtest'\n");
        for (;;) { asm volatile("cli; hlt"); }
    }
    static const char *const argv[] = { "argvtest", "hello" };
    tcb_t *t = elf_load_and_spawn_argv("argvtest", elf_start, elf_end, 9, PAGER_TID, 2, argv);
    if (!t) {
        kprintf("[boot] FATAL: argvtest failed to load\n");
        for (;;) { asm volatile("cli; hlt"); }
    }
    kprintf("[boot] argvtest: tid=%u\n", t->tid);
}

static uint8_t stack_panic_test[STACK_SIZE] __attribute__((aligned(16)));
static void __attribute__((noinline)) panic_test_fault(void) {
    asm volatile("ud2");
}
static void panic_test_entry(void) {
    kputs("[boot] deliberately triggering a kernel-mode fault...");
    panic_test_fault();
}
void panic_test_init(void) {
    if (!cmdline_get("panic_test")) {
        return;
    }
    thread_create("panic-test", panic_test_entry, stack_panic_test + STACK_SIZE, 9);
}

static uint8_t stack_test_exit[STACK_SIZE] __attribute__((aligned(16)));
static int test_exit_code;
static int test_exit_delay_secs = 3;
static void test_exit_entry(void) {
    ipc_sleep((uint64_t)(SCHED_HZ * test_exit_delay_secs));
    kprintf("[boot] exiting with code %d\n", test_exit_code);
    arch_test_exit(test_exit_code);
}
static int parse_uint(const char *val) {
    int v = 0;
    while (*val >= '0' && *val <= '9') {
        v = v * 10 + (*val - '0');
        val++;
    }
    return v;
}
void test_exit_init(void) {
    const char *val = cmdline_get("test_exit");
    if (!val) {
        return;
    }
    int code = parse_uint(val);
    if (code > 255) {
        code = 255;
    }
    test_exit_code = code;

    const char *delay_val = cmdline_get("test_exit_delay");
    if (delay_val) {
        test_exit_delay_secs = parse_uint(delay_val);
    }
    thread_create("test-exit", test_exit_entry, stack_test_exit + STACK_SIZE, 9);
}

void libctest_init(void) {
    if (!cmdline_get("libctest")) {
        return;
    }
    const uint8_t *elf_start, *elf_end;
    if (rootfs_lookup("libctest", &elf_start, &elf_end) != 0) {
        kprintf("[boot] FATAL: rootfs has no entry named 'libctest'\n");
        for (;;) { asm volatile("cli; hlt"); }
    }
    static const char *const argv[] = { "libctest" };
    tcb_t *t = elf_load_and_spawn_argv("libctest", elf_start, elf_end, 9, PAGER_TID, 1, argv);
    if (!t) {
        kprintf("[boot] FATAL: libctest failed to load\n");
        for (;;) { asm volatile("cli; hlt"); }
    }
    kprintf("[boot] libctest: tid=%u\n", t->tid);
}

void ramfstest_init(void) {
    if (!cmdline_get("ramfstest")) {
        return;
    }
    const uint8_t *elf_start, *elf_end;
    if (rootfs_lookup("ramfstest", &elf_start, &elf_end) != 0) {
        kprintf("[boot] FATAL: rootfs has no entry named 'ramfstest'\n");
        for (;;) { asm volatile("cli; hlt"); }
    }
    static const char *const argv[] = { "ramfstest" };
    tcb_t *t = elf_load_and_spawn_argv("ramfstest", elf_start, elf_end, 9, PAGER_TID, 1, argv);
    if (!t) {
        kprintf("[boot] FATAL: ramfstest failed to load\n");
        for (;;) { asm volatile("cli; hlt"); }
    }
    kprintf("[boot] ramfstest: tid=%u\n", t->tid);
}

void spawntest_init(void) {
    if (!cmdline_get("spawntest")) {
        return;
    }
    toybox_spawn("spawntest", 1, (const char *const[]){ "spawntest" }, 9);
}

void sigtest_init(void) {
    if (!cmdline_get("sigtest")) {
        return;
    }
    toybox_spawn("sigtest", 1, (const char *const[]){ "sigtest" }, 9);
}

void consoletest_init(void) {
    if (!cmdline_get("consoletest")) {
        return;
    }
    toybox_spawn("consoletest", 1, (const char *const[]){ "consoletest" }, 9);
}

void readlinetest_init(void) {
    if (!cmdline_get("readlinetest")) {
        return;
    }
    toybox_spawn("readlinetest", 1, (const char *const[]){ "readlinetest" }, 9);
}

void toybox_sh_c_init(void) {
    const char *cmd = cmdline_get("toybox_sh_c");
    if (!cmd) {
        return;
    }
    toybox_spawn("sh", 3, (const char *const[]){ "sh", "-c", cmd }, 9);
}
