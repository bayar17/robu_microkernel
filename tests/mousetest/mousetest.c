#include "robu/types.h"
#include "robu/uipc.h"
#include "robu/ipc.h"
#include "robu/vfs.h"
#include "robu/kinfo.h"
#include "robu/testreport.h"

typedef struct __attribute__((packed)) {
    int16_t dx;
    int16_t dy;
    uint8_t buttons;
    uint8_t reserved[3];
} mouse_event_t;

void _start(void) {
    tid_t devfs = (tid_t)kinfo_user()->devfs_tid;
    int64_t h = vfs_open(devfs, "mouse", 0);

    int checks = 0, passed = 0;
    mouse_event_t last = {0};

    for (int i = 0; i < 3; i++) {
        mouse_event_t ev = {0};
        int64_t n;
        do {
            n = vfs_read(devfs, (uint64_t)h, (uint8_t *)&ev, sizeof(ev));
        } while (n <= 0);
        checks++;
        if (n == (int64_t)sizeof(ev)) {
            passed++;
            last = ev;
        }
    }

    vfs_close(devfs, (uint64_t)h);

    uint64_t packed_last = ((uint64_t)(uint16_t)last.dx & 0xFFFF)
                          | (((uint64_t)(uint16_t)last.dy & 0xFFFF) << 16)
                          | ((uint64_t)last.buttons << 32);

    msg_regs_t rep = (msg_regs_t){0};
    rep.word[0] = TEST_REPORT_KIND_MOUSE_TEST;
    rep.word[1] = (uint64_t)checks;
    rep.word[2] = (uint64_t)passed;
    rep.word[3] = (uint64_t)(checks - passed);
    rep.word[4] = packed_last;
    tid_t test_report_tid = (tid_t)kinfo_user()->test_report_tid;
    ipc_send(test_report_tid, &rep);

    ipc_exit(passed == checks ? 0 : 1);
}
