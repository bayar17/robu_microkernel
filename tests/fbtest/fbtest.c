#include "robu/types.h"
#include "robu/uipc.h"
#include "robu/ipc.h"
#include "robu/kinfo.h"
#include "robu/testreport.h"
#include "robu/framebuffer.h"
#include "robu/kprintf.h"

void _start(void) {
    msg_regs_t m = (msg_regs_t){0};
    m.word[0] = SYS_INFO_CAT_FB_MAP;
    int64_t rc = robu_ipc_raw(0, 0, IPC_FLAG_SYS_INFO, &m, NULL);

    int checks = 1;
    int passed = (rc == IPC_ERR_NONE) ? 1 : 0;
    uint32_t width = 0, height = 0;

    if (passed) {
        width = (uint32_t)m.word[0];
        height = (uint32_t)m.word[1];
        uint32_t pitch = (uint32_t)m.word[2];
        uint64_t packed = m.word[3];
        uint8_t bpp = (uint8_t)packed;
        uint8_t red_pos = (uint8_t)(packed >> 8);
        uint8_t green_pos = (uint8_t)(packed >> 24);
        uint8_t blue_pos = (uint8_t)(packed >> 40);

        checks++;
        if (bpp == 32 && width > 0 && height > 0) {
            passed++;
            volatile uint8_t *fb = (volatile uint8_t *)FRAMEBUFFER_USER_VA;
            for (uint32_t y = 0; y < height; y++) {
                uint32_t stripe = (y * 8) / height;
                uint8_t r = (stripe & 4) ? 255 : 0;
                uint8_t g = (stripe & 2) ? 255 : 0;
                uint8_t b = (stripe & 1) ? 255 : 0;
                uint32_t pixel = ((uint32_t)r << red_pos)
                                | ((uint32_t)g << green_pos)
                                | ((uint32_t)b << blue_pos);
                volatile uint32_t *row = (volatile uint32_t *)(fb + (uint64_t)y * pitch);
                for (uint32_t x = 0; x < width; x++) {
                    row[x] = pixel;
                }
            }
        }
    }

    msg_regs_t rep = (msg_regs_t){0};
    rep.word[0] = TEST_REPORT_KIND_FB_TEST;
    rep.word[1] = (uint64_t)checks;
    rep.word[2] = (uint64_t)passed;
    rep.word[3] = (uint64_t)(checks - passed);
    rep.word[4] = ((uint64_t)width & 0xFFFF) | (((uint64_t)height & 0xFFFF) << 16);
    tid_t test_report_tid = (tid_t)kinfo_user()->test_report_tid;
    ipc_send(test_report_tid, &rep);

    ipc_exit(passed == checks ? 0 : 1);
}
