#include "robu/types.h"
#include "robu/uipc.h"
#include "robu/ipc.h"
#include "robu/blockinfo.h"
#include "block.h"
#ifdef EXT2FS_AS_ROOT
#include "../../xhci.h"
#endif

#define SYS_INFO_CAT_HW_PORT_IO 38
#define SYS_INFO_CAT_HW_PORT_IO_BULK_READ 40
#define SYS_INFO_CAT_HW_PORT_IO_BULK_WRITE 41
#define HW_PORT_IO_BULK_READ_MAX 40
#define HW_PORT_IO_BULK_WRITE_MAX 24

#ifdef EXT2FS_AS_ROOT
#define EXT2_PARTITION_START_LBA 2048
#else
#define EXT2_PARTITION_START_LBA 0
#endif

#define IDE_PRIMARY_IO_BASE   0x1F0
#define IDE_DRIVE_SELECT_MASTER 0xE0
#define IDE_REG_DATA          0x00
#define IDE_REG_SECCOUNT      0x02
#define IDE_REG_LBA_LOW       0x03
#define IDE_REG_LBA_MID       0x04
#define IDE_REG_LBA_HIGH      0x05
#define IDE_REG_DRIVE_HEAD    0x06
#define IDE_REG_STATUS        0x07
#define IDE_REG_COMMAND       0x07
#define IDE_STATUS_ERR  0x01
#define IDE_STATUS_DRQ  0x08
#define IDE_STATUS_BSY  0x80
#define IDE_CMD_READ_SECTORS  0x20
#define IDE_CMD_WRITE_SECTORS 0x30
#define IDE_CMD_FLUSH_CACHE   0xE7
#define IDE_CMD_IDENTIFY      0xEC
#define IDE_POLL_MAX_ITERS 200000

static int g_present;
static uint64_t g_capacity_sectors;
#ifdef EXT2FS_AS_ROOT
static int g_xhci_present;
#endif

static int64_t hw_io(uint16_t port, int width, int is_write, uint64_t value) {
    msg_regs_t m = (msg_regs_t){0};
    m.word[0] = SYS_INFO_CAT_HW_PORT_IO;
    m.word[1] = port;
    m.word[2] = (uint64_t)width;
    m.word[3] = (uint64_t)is_write;
    m.word[4] = value;
    int64_t rc = robu_ipc_raw(0, 0, IPC_FLAG_SYS_INFO, &m, NULL);
    if (rc != IPC_ERR_NONE) {
        return -1;
    }
    return (int64_t)m.word[0];
}
static void hw_outb(uint16_t port, uint8_t v) { hw_io(port, 1, 1, v); }
static uint8_t hw_inb(uint16_t port) { return (uint8_t)hw_io(port, 1, 0, 0); }
static uint16_t hw_inw(uint16_t port) { return (uint16_t)hw_io(port, 2, 0, 0); }

static int hw_bulk_read(uint16_t port, uint8_t *out, int count) {
    msg_regs_t m = (msg_regs_t){0};
    m.word[0] = SYS_INFO_CAT_HW_PORT_IO_BULK_READ;
    m.word[1] = port;
    m.word[2] = (uint64_t)count;
    int64_t rc = robu_ipc_raw(0, 0, IPC_FLAG_SYS_INFO, &m, NULL);
    if (rc != IPC_ERR_NONE) {
        return -1;
    }
    int64_t got = (int64_t)m.word[0];
    if (got <= 0) {
        return -1;
    }
    uint64_t regs[5] = {m.word[1], m.word[2], m.word[3], m.word[4], m.word[5]};
    for (int64_t i = 0; i < got; i++) {
        out[i] = ((uint8_t *)regs)[i];
    }
    return (int)got;
}

static int hw_bulk_write(uint16_t port, const uint8_t *in, int count) {
    msg_regs_t m = (msg_regs_t){0};
    m.word[0] = SYS_INFO_CAT_HW_PORT_IO_BULK_WRITE;
    m.word[1] = port;
    m.word[2] = (uint64_t)count;
    uint64_t regs[3] = {0, 0, 0};
    for (int i = 0; i < count; i++) {
        ((uint8_t *)regs)[i] = in[i];
    }
    m.word[3] = regs[0];
    m.word[4] = regs[1];
    m.word[5] = regs[2];
    int64_t rc = robu_ipc_raw(0, 0, IPC_FLAG_SYS_INFO, &m, NULL);
    if (rc != IPC_ERR_NONE) {
        return -1;
    }
    int64_t got = (int64_t)m.word[0];
    if (got <= 0) {
        return -1;
    }
    return (int)got;
}

static int ide_wait_status(uint8_t want_set, uint8_t want_clear, uint8_t *out_status) {
    for (int i = 0; i < IDE_POLL_MAX_ITERS; i++) {
        uint8_t status = hw_inb(IDE_PRIMARY_IO_BASE + IDE_REG_STATUS);
        if ((status & want_clear) == 0 && (status & want_set) == want_set) {
            *out_status = status;
            return 0;
        }
        if (status & IDE_STATUS_ERR) {
            *out_status = status;
            return -1;
        }
    }
    *out_status = 0;
    return -1;
}

static int ide_probe(void) {
    hw_outb(IDE_PRIMARY_IO_BASE + IDE_REG_DRIVE_HEAD, IDE_DRIVE_SELECT_MASTER);
    uint8_t status = hw_inb(IDE_PRIMARY_IO_BASE + IDE_REG_STATUS);
    if (status == 0xFF) {
        return -1;
    }

    hw_outb(IDE_PRIMARY_IO_BASE + IDE_REG_SECCOUNT, 0);
    hw_outb(IDE_PRIMARY_IO_BASE + IDE_REG_LBA_LOW, 0);
    hw_outb(IDE_PRIMARY_IO_BASE + IDE_REG_LBA_MID, 0);
    hw_outb(IDE_PRIMARY_IO_BASE + IDE_REG_LBA_HIGH, 0);
    hw_outb(IDE_PRIMARY_IO_BASE + IDE_REG_COMMAND, IDE_CMD_IDENTIFY);

    status = hw_inb(IDE_PRIMARY_IO_BASE + IDE_REG_STATUS);
    if (status == 0) {
        return -1;
    }

    uint8_t lba_mid = hw_inb(IDE_PRIMARY_IO_BASE + IDE_REG_LBA_MID);
    uint8_t lba_high = hw_inb(IDE_PRIMARY_IO_BASE + IDE_REG_LBA_HIGH);
    if (lba_mid == 0x14 && lba_high == 0xEB) {
        return -1;
    }

    if (ide_wait_status(IDE_STATUS_DRQ, IDE_STATUS_BSY, &status) != 0) {
        return -1;
    }

    uint16_t identify[256];
    for (int i = 0; i < 256; i++) {
        identify[i] = hw_inw(IDE_PRIMARY_IO_BASE + IDE_REG_DATA);
    }
    g_capacity_sectors = (uint64_t)identify[60] | ((uint64_t)identify[61] << 16);
    if (g_capacity_sectors == 0) {
        return -1;
    }
    g_present = 1;
    return 0;
}

int blkdev_probe(void) {
#ifdef EXT2FS_AS_ROOT
    if (xhci_probe() == 0) {
        g_xhci_present = 1;
        g_capacity_sectors = xhci_capacity_sectors();
        return 0;
    }
#endif
    if (ide_probe() == 0) {
        return 0;
    }
#ifdef EXT2FS_AS_ROOT
    return -xhci_last_failure();
#else
    return -1;
#endif
}

int blkdev_raw_read(uint64_t sector, uint32_t count, void *buf) {
#ifdef EXT2FS_AS_ROOT
    if (g_xhci_present) {
        int rc = xhci_read(sector, count, buf);
        return rc == 0 ? 0 : -xhci_last_failure();
    }
#endif
    if (!g_present) {
        return -1;
    }
    uint8_t *bytes = (uint8_t *)buf;
    for (uint32_t s = 0; s < count; s++) {
        uint64_t lba = sector + s;
        uint8_t status;
        if (ide_wait_status(0, IDE_STATUS_BSY, &status) != 0) {
            return -1;
        }
        hw_outb(IDE_PRIMARY_IO_BASE + IDE_REG_DRIVE_HEAD,
                (uint8_t)(IDE_DRIVE_SELECT_MASTER | ((lba >> 24) & 0x0F)));
        hw_outb(IDE_PRIMARY_IO_BASE + IDE_REG_SECCOUNT, 1);
        hw_outb(IDE_PRIMARY_IO_BASE + IDE_REG_LBA_LOW, (uint8_t)(lba & 0xFF));
        hw_outb(IDE_PRIMARY_IO_BASE + IDE_REG_LBA_MID, (uint8_t)((lba >> 8) & 0xFF));
        hw_outb(IDE_PRIMARY_IO_BASE + IDE_REG_LBA_HIGH, (uint8_t)((lba >> 16) & 0xFF));
        hw_outb(IDE_PRIMARY_IO_BASE + IDE_REG_COMMAND, IDE_CMD_READ_SECTORS);

        if (ide_wait_status(IDE_STATUS_DRQ, IDE_STATUS_BSY, &status) != 0) {
            return -1;
        }

        uint8_t *sector_buf = bytes + (uint64_t)s * 512;
        int off = 0;
        while (off < 512) {
            int chunk = 512 - off;
            if (chunk > HW_PORT_IO_BULK_READ_MAX) {
                chunk = HW_PORT_IO_BULK_READ_MAX;
            }
            int got = hw_bulk_read(IDE_PRIMARY_IO_BASE + IDE_REG_DATA, sector_buf + off, chunk);
            if (got <= 0) {
                return -1;
            }
            off += got;
        }

        if (ide_wait_status(0, IDE_STATUS_BSY, &status) != 0) {
            return -1;
        }
    }
    return 0;
}

int blkdev_read(uint64_t sector, uint32_t count, void *buf) {
    return blkdev_raw_read(EXT2_PARTITION_START_LBA + sector, count, buf);
}

int blkdev_raw_write(uint64_t sector, uint32_t count, const void *buf) {
#ifdef EXT2FS_AS_ROOT
    if (g_xhci_present) {
        int rc = xhci_write(sector, count, buf);
        return rc == 0 ? 0 : -xhci_last_failure();
    }
#endif
    if (!g_present) {
        return -1;
    }
    const uint8_t *bytes = (const uint8_t *)buf;
    for (uint32_t s = 0; s < count; s++) {
        uint64_t lba = sector + s;
        uint8_t status;
        if (ide_wait_status(0, IDE_STATUS_BSY, &status) != 0) {
            return -1;
        }
        hw_outb(IDE_PRIMARY_IO_BASE + IDE_REG_DRIVE_HEAD,
                (uint8_t)(IDE_DRIVE_SELECT_MASTER | ((lba >> 24) & 0x0F)));
        hw_outb(IDE_PRIMARY_IO_BASE + IDE_REG_SECCOUNT, 1);
        hw_outb(IDE_PRIMARY_IO_BASE + IDE_REG_LBA_LOW, (uint8_t)(lba & 0xFF));
        hw_outb(IDE_PRIMARY_IO_BASE + IDE_REG_LBA_MID, (uint8_t)((lba >> 8) & 0xFF));
        hw_outb(IDE_PRIMARY_IO_BASE + IDE_REG_LBA_HIGH, (uint8_t)((lba >> 16) & 0xFF));
        hw_outb(IDE_PRIMARY_IO_BASE + IDE_REG_COMMAND, IDE_CMD_WRITE_SECTORS);

        if (ide_wait_status(IDE_STATUS_DRQ, IDE_STATUS_BSY, &status) != 0) {
            return -1;
        }

        const uint8_t *sector_buf = bytes + (uint64_t)s * 512;
        int off = 0;
        while (off < 512) {
            int chunk = 512 - off;
            if (chunk > HW_PORT_IO_BULK_WRITE_MAX) {
                chunk = HW_PORT_IO_BULK_WRITE_MAX;
            }
            int got = hw_bulk_write(IDE_PRIMARY_IO_BASE + IDE_REG_DATA, sector_buf + off, chunk);
            if (got <= 0) {
                return -1;
            }
            off += got;
        }

        if (ide_wait_status(0, IDE_STATUS_BSY, &status) != 0) {
            return -1;
        }
    }
    return 0;
}

int blkdev_write(uint64_t sector, uint32_t count, const void *buf) {
    return blkdev_raw_write(EXT2_PARTITION_START_LBA + sector, count, buf);
}

int blkdev_flush(void) {
#ifdef EXT2FS_AS_ROOT
    if (g_xhci_present) {
        int rc = xhci_flush();
        return rc == 0 ? 0 : -xhci_last_failure();
    }
#endif
    if (!g_present) {
        return -1;
    }
    uint8_t status;
    if (ide_wait_status(0, IDE_STATUS_BSY, &status) != 0) {
        return -1;
    }
    hw_outb(IDE_PRIMARY_IO_BASE + IDE_REG_DRIVE_HEAD, IDE_DRIVE_SELECT_MASTER);
    hw_outb(IDE_PRIMARY_IO_BASE + IDE_REG_COMMAND, IDE_CMD_FLUSH_CACHE);
    return ide_wait_status(0, IDE_STATUS_BSY, &status);
}

uint64_t blkdev_capacity_sectors(void) {
    return g_capacity_sectors;
}
uint32_t blkdev_transport(void) {
#ifdef EXT2FS_AS_ROOT
    if (g_xhci_present) {
        return BLOCK_TRANSPORT_XHCI;
    }
#endif
    return BLOCK_TRANSPORT_IDE;
}
