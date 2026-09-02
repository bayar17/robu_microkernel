#include "robu/types.h"
#include "robu/uipc.h"
#include "robu/ipc.h"
#include "block.h"

#define SYS_INFO_CAT_HW_PORT_IO 38

#define IDE_SECONDARY_IO_BASE 0x170
#define IDE_DRIVE_SELECT_SLAVE 0xF0
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
#define IDE_CMD_IDENTIFY      0xEC
#define IDE_POLL_MAX_ITERS 200000

static int g_present;
static uint64_t g_capacity_sectors;

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
static void hw_outw(uint16_t port, uint16_t v) { hw_io(port, 2, 1, v); }
static uint8_t hw_inb(uint16_t port) { return (uint8_t)hw_io(port, 1, 0, 0); }
static uint16_t hw_inw(uint16_t port) { return (uint16_t)hw_io(port, 2, 0, 0); }

static int ide_wait_status(uint8_t want_set, uint8_t want_clear, uint8_t *out_status) {
    for (int i = 0; i < IDE_POLL_MAX_ITERS; i++) {
        uint8_t status = hw_inb(IDE_SECONDARY_IO_BASE + IDE_REG_STATUS);
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
int blkdev_probe(void) {
    hw_outb(IDE_SECONDARY_IO_BASE + IDE_REG_DRIVE_HEAD, IDE_DRIVE_SELECT_SLAVE);
    uint8_t status = hw_inb(IDE_SECONDARY_IO_BASE + IDE_REG_STATUS);
    if (status == 0xFF) {
        return -1;
    }

    hw_outb(IDE_SECONDARY_IO_BASE + IDE_REG_SECCOUNT, 0);
    hw_outb(IDE_SECONDARY_IO_BASE + IDE_REG_LBA_LOW, 0);
    hw_outb(IDE_SECONDARY_IO_BASE + IDE_REG_LBA_MID, 0);
    hw_outb(IDE_SECONDARY_IO_BASE + IDE_REG_LBA_HIGH, 0);
    hw_outb(IDE_SECONDARY_IO_BASE + IDE_REG_COMMAND, IDE_CMD_IDENTIFY);

    status = hw_inb(IDE_SECONDARY_IO_BASE + IDE_REG_STATUS);
    if (status == 0) {
        return -1;
    }

    uint8_t lba_mid = hw_inb(IDE_SECONDARY_IO_BASE + IDE_REG_LBA_MID);
    uint8_t lba_high = hw_inb(IDE_SECONDARY_IO_BASE + IDE_REG_LBA_HIGH);
    if (lba_mid == 0x14 && lba_high == 0xEB) {
        return -1;
    }

    if (ide_wait_status(IDE_STATUS_DRQ, IDE_STATUS_BSY, &status) != 0) {
        return -1;
    }

    uint16_t identify[256];
    for (int i = 0; i < 256; i++) {
        identify[i] = hw_inw(IDE_SECONDARY_IO_BASE + IDE_REG_DATA);
    }
    g_capacity_sectors = (uint64_t)identify[60] | ((uint64_t)identify[61] << 16);
    if (g_capacity_sectors == 0) {
        return -1;
    }
    g_present = 1;
    return 0;
}

int blkdev_read(uint64_t sector, uint32_t count, void *buf) {
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
        hw_outb(IDE_SECONDARY_IO_BASE + IDE_REG_DRIVE_HEAD,
                (uint8_t)(IDE_DRIVE_SELECT_SLAVE | ((lba >> 24) & 0x0F)));
        hw_outb(IDE_SECONDARY_IO_BASE + IDE_REG_SECCOUNT, 1);
        hw_outb(IDE_SECONDARY_IO_BASE + IDE_REG_LBA_LOW, (uint8_t)(lba & 0xFF));
        hw_outb(IDE_SECONDARY_IO_BASE + IDE_REG_LBA_MID, (uint8_t)((lba >> 8) & 0xFF));
        hw_outb(IDE_SECONDARY_IO_BASE + IDE_REG_LBA_HIGH, (uint8_t)((lba >> 16) & 0xFF));
        hw_outb(IDE_SECONDARY_IO_BASE + IDE_REG_COMMAND, IDE_CMD_READ_SECTORS);

        if (ide_wait_status(IDE_STATUS_DRQ, IDE_STATUS_BSY, &status) != 0) {
            return -1;
        }

        uint8_t *sector_buf = bytes + (uint64_t)s * 512;
        for (int i = 0; i < 256; i++) {
            uint16_t w = hw_inw(IDE_SECONDARY_IO_BASE + IDE_REG_DATA);
            sector_buf[i * 2] = (uint8_t)w;
            sector_buf[i * 2 + 1] = (uint8_t)(w >> 8);
        }

        if (ide_wait_status(0, IDE_STATUS_BSY, &status) != 0) {
            return -1;
        }
    }
    return 0;
}

int blkdev_write(uint64_t sector, uint32_t count, const void *buf) {
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
        hw_outb(IDE_SECONDARY_IO_BASE + IDE_REG_DRIVE_HEAD,
                (uint8_t)(IDE_DRIVE_SELECT_SLAVE | ((lba >> 24) & 0x0F)));
        hw_outb(IDE_SECONDARY_IO_BASE + IDE_REG_SECCOUNT, 1);
        hw_outb(IDE_SECONDARY_IO_BASE + IDE_REG_LBA_LOW, (uint8_t)(lba & 0xFF));
        hw_outb(IDE_SECONDARY_IO_BASE + IDE_REG_LBA_MID, (uint8_t)((lba >> 8) & 0xFF));
        hw_outb(IDE_SECONDARY_IO_BASE + IDE_REG_LBA_HIGH, (uint8_t)((lba >> 16) & 0xFF));
        hw_outb(IDE_SECONDARY_IO_BASE + IDE_REG_COMMAND, IDE_CMD_WRITE_SECTORS);

        if (ide_wait_status(IDE_STATUS_DRQ, IDE_STATUS_BSY, &status) != 0) {
            return -1;
        }

        const uint8_t *sector_buf = bytes + (uint64_t)s * 512;
        for (int i = 0; i < 256; i++) {
            uint16_t w = (uint16_t)sector_buf[i * 2] | ((uint16_t)sector_buf[i * 2 + 1] << 8);
            hw_outw(IDE_SECONDARY_IO_BASE + IDE_REG_DATA, w);
        }

        if (ide_wait_status(0, IDE_STATUS_BSY, &status) != 0) {
            return -1;
        }
    }
    return 0;
}

uint64_t blkdev_capacity_sectors(void) {
    return g_capacity_sectors;
}
