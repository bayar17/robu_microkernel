#include "robu/types.h"
#include "robu/uipc.h"
#include "robu/ipc.h"
#include "robu/kinfo.h"
#include "robu/testreport.h"
#include "robu/hwdrv.h"

static int64_t pci_cfg(uint8_t bus, uint8_t device, uint8_t function,
                        uint8_t offset, int width, int is_write, uint64_t value) {
    msg_regs_t m = (msg_regs_t){0};
    m.word[0] = SYS_INFO_CAT_PCI_CFG;
    m.word[1] = ((uint64_t)bus << 16) | ((uint64_t)device << 8) | (uint64_t)function;
    m.word[2] = (uint64_t)offset | ((uint64_t)width << 8) | ((uint64_t)is_write << 16);
    m.word[3] = value;
    int64_t rc = robu_ipc_raw(0, 0, IPC_FLAG_SYS_INFO, &m, NULL);
    if (rc != IPC_ERR_NONE) {
        return -1;
    }
    return (int64_t)m.word[0];
}

void _start(void) {
    int found = 0;
    uint8_t found_bus = 0, found_dev = 0, found_fn = 0;

    for (int bus = 0; bus < 256 && !found; bus++) {
        for (int dev = 0; dev < 32 && !found; dev++) {
            int64_t vid0 = pci_cfg((uint8_t)bus, (uint8_t)dev, 0, 0x00, 2, 0, 0);
            if (vid0 < 0 || (uint16_t)vid0 == 0xFFFF) {
                continue;
            }
            int64_t header_type = pci_cfg((uint8_t)bus, (uint8_t)dev, 0, 0x0E, 1, 0, 0);
            int nfuncs = (header_type >= 0 && (header_type & 0x80)) ? 8 : 1;
            for (int fn = 0; fn < nfuncs; fn++) {
                int64_t vid = pci_cfg((uint8_t)bus, (uint8_t)dev, (uint8_t)fn, 0x00, 2, 0, 0);
                if (vid < 0 || (uint16_t)vid == 0xFFFF) {
                    continue;
                }
                int64_t did = pci_cfg((uint8_t)bus, (uint8_t)dev, (uint8_t)fn, 0x02, 2, 0, 0);
                if ((uint16_t)vid == 0x1af4 && (uint16_t)did == 0x1001) {
                    found = 1;
                    found_bus = (uint8_t)bus;
                    found_dev = (uint8_t)dev;
                    found_fn = (uint8_t)fn;
                    break;
                }
            }
        }
    }

    int checks = 1;
    int passed = found ? 1 : 0;

    msg_regs_t rep = (msg_regs_t){0};
    rep.word[0] = TEST_REPORT_KIND_PCI_TEST;
    rep.word[1] = (uint64_t)checks;
    rep.word[2] = (uint64_t)passed;
    rep.word[3] = (uint64_t)(checks - passed);
    rep.word[4] = ((uint64_t)found_bus) | ((uint64_t)found_dev << 8) | ((uint64_t)found_fn << 16);
    tid_t test_report_tid = (tid_t)kinfo_user()->test_report_tid;
    ipc_send(test_report_tid, &rep);

    ipc_exit(passed == checks ? 0 : 1);
}
