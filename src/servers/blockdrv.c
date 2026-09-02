#include "robu/types.h"
#include "robu/uipc.h"
#include "robu/ipc.h"
#include "robu/blockinfo.h"
#include "robu/kprintf.h"

#define SYS_INFO_CAT_PCI_CFG    37
#define SYS_INFO_CAT_HW_PORT_IO 38
#define SYS_INFO_CAT_DMA_ALLOC  39

#define BLOCKDRV_AF_UNIX 1
#define BLOCKDRV_SOCK_STREAM 1
#define BLOCKDRV_SOCK_PATH "/tmp/.blockdrv"
#define BLOCKDRV_SHM_KEY 0x424C4B44
#define BLOCKDRV_IPC_CREAT 01000
#define BLOCKDRV_BLOCK_SIZE 4096
#define BLOCKDRV_OP_READ  0
#define BLOCKDRV_OP_WRITE 1

#define VIRTIO_REG_DEVICE_FEATURES 0x00
#define VIRTIO_REG_GUEST_FEATURES  0x04
#define VIRTIO_REG_QUEUE_ADDRESS   0x08
#define VIRTIO_REG_QUEUE_SIZE      0x0C
#define VIRTIO_REG_QUEUE_SELECT    0x0E
#define VIRTIO_REG_QUEUE_NOTIFY    0x10
#define VIRTIO_REG_DEVICE_STATUS   0x12
#define VIRTIO_REG_ISR_STATUS      0x13
#define VIRTIO_REG_CONFIG          0x14
#define VIRTIO_STATUS_ACKNOWLEDGE 0x01
#define VIRTIO_STATUS_DRIVER      0x02
#define VIRTIO_STATUS_DRIVER_OK   0x04
#define VIRTIO_STATUS_FAILED      0x80
#define VIRTQ_DESC_F_NEXT  1
#define VIRTQ_DESC_F_WRITE 2
#define VIRTIO_BLK_T_IN  0
#define VIRTIO_BLK_T_OUT 1
#define VIRTIO_BLK_S_OK 0
#define VIRTIO_BLK_DATA_MAX 4096
#define VIRTIO_BLK_POLL_MAX_ITERS 10000000
#define VIRTIO_BLK_MAX_QUEUE_SIZE 1024

#define IDE_PRIMARY_IO_BASE   0x1F0
#define IDE_REG_DATA          0x00
#define IDE_REG_ERROR         0x01
#define IDE_REG_SECCOUNT      0x02
#define IDE_REG_LBA_LOW       0x03
#define IDE_REG_LBA_MID       0x04
#define IDE_REG_LBA_HIGH      0x05
#define IDE_REG_DRIVE_HEAD    0x06
#define IDE_REG_STATUS        0x07
#define IDE_REG_COMMAND       0x07
#define IDE_STATUS_ERR  0x01
#define IDE_STATUS_DRQ  0x08
#define IDE_STATUS_DRDY 0x40
#define IDE_STATUS_BSY  0x80
#define IDE_CMD_READ_SECTORS  0x20
#define IDE_CMD_WRITE_SECTORS 0x30
#define IDE_CMD_IDENTIFY      0xEC
#define IDE_POLL_MAX_ITERS 200000

#define BLK_BACKEND_NONE   0
#define BLK_BACKEND_VIRTIO 1
#define BLK_BACKEND_IDE    2

typedef struct {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} __attribute__((packed)) virtq_desc_t;
typedef struct {
    uint32_t type;
    uint32_t reserved;
    uint64_t sector;
} __attribute__((packed)) virtio_blk_req_hdr_t;

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
static void hw_outl(uint16_t port, uint32_t v) { hw_io(port, 4, 1, v); }
static uint8_t hw_inb(uint16_t port) { return (uint8_t)hw_io(port, 1, 0, 0); }
static uint16_t hw_inw(uint16_t port) { return (uint16_t)hw_io(port, 2, 0, 0); }
static uint32_t hw_inl(uint16_t port) { return (uint32_t)hw_io(port, 4, 0, 0); }

static int dma_alloc(uint64_t bytes, uint64_t *out_va, uint64_t *out_pa) {
    msg_regs_t m = (msg_regs_t){0};
    m.word[0] = SYS_INFO_CAT_DMA_ALLOC;
    m.word[1] = bytes;
    int64_t rc = robu_ipc_raw(0, 0, IPC_FLAG_SYS_INFO, &m, NULL);
    if (rc != IPC_ERR_NONE) {
        return -1;
    }
    *out_va = m.word[0];
    *out_pa = m.word[1];
    return 0;
}

static int g_backend = BLK_BACKEND_NONE;
static uint64_t ide_capacity_sectors;

static int present;
static uint16_t io_base;
static uint64_t capacity_sectors;
static uint32_t queue_size;
static volatile virtq_desc_t *desc;
static volatile uint8_t *avail_base;
static volatile uint8_t *used_base;
static uint16_t next_avail_idx;
static uint16_t last_seen_used_idx;
static volatile virtio_blk_req_hdr_t *req_hdr;
static uint64_t req_hdr_pa;
static volatile uint8_t *req_status;
static uint64_t req_status_pa;
static volatile uint8_t *req_data;
static uint64_t req_data_pa;

static void publish_block_info(void) {
    uint32_t transport = BLOCK_TRANSPORT_NONE;
    uint64_t sectors = 0;
    if (g_backend == BLK_BACKEND_VIRTIO) {
        transport = BLOCK_TRANSPORT_VIRTIO;
        sectors = capacity_sectors;
    } else if (g_backend == BLK_BACKEND_IDE) {
        transport = BLOCK_TRANSPORT_IDE;
        sectors = ide_capacity_sectors;
    }
    if (transport == BLOCK_TRANSPORT_NONE || sectors == 0) {
        return;
    }
    msg_regs_t m = (msg_regs_t){0};
    m.word[0] = SYS_INFO_CAT_BLOCK_INFO;
    m.word[1] = BLOCK_INFO_OP_PUBLISH;
    m.word[2] = BLOCK_DEVICE_DATA;
    m.word[3] = ((uint64_t)transport << 32) | BLOCK_FS_DISKFS;
    m.word[4] = sectors;
    m.word[5] = 512;
    robu_ipc_raw(0, 0, IPC_FLAG_SYS_INFO, &m, NULL);
}

static inline volatile uint16_t *avail_idx_ptr(void) {
    return (volatile uint16_t *)(avail_base + 2);
}
static inline volatile uint16_t *avail_ring_ptr(uint32_t i) {
    return (volatile uint16_t *)(avail_base + 4 + 2 * i);
}
static inline volatile uint16_t *used_idx_ptr(void) {
    return (volatile uint16_t *)(used_base + 2);
}

static int poll_for_completion(void) {
    for (uint64_t i = 0; i < VIRTIO_BLK_POLL_MAX_ITERS; i++) {
        if (*used_idx_ptr() != last_seen_used_idx) {
            last_seen_used_idx = *used_idx_ptr();
            return 0;
        }
        asm volatile("pause");
    }
    return -1;
}

static int do_request(uint32_t type, uint64_t sector, uint32_t count, void *buf, int is_write) {
    if (!present) {
        return -1;
    }
    uint32_t bytes = count * 512;
    if (bytes > VIRTIO_BLK_DATA_MAX) {
        return -1;
    }
    req_hdr->type = type;
    req_hdr->reserved = 0;
    req_hdr->sector = sector;
    if (is_write) {
        uint8_t *dst = (uint8_t *)req_data;
        const uint8_t *src = (const uint8_t *)buf;
        for (uint32_t i = 0; i < bytes; i++) {
            dst[i] = src[i];
        }
    }
    *req_status = 0xFF;
    desc[0].addr = req_hdr_pa;
    desc[0].len = sizeof(virtio_blk_req_hdr_t);
    desc[0].flags = VIRTQ_DESC_F_NEXT;
    desc[0].next = 1;
    desc[1].addr = req_data_pa;
    desc[1].len = bytes;
    desc[1].flags = VIRTQ_DESC_F_NEXT | (is_write ? 0 : VIRTQ_DESC_F_WRITE);
    desc[1].next = 2;
    desc[2].addr = req_status_pa;
    desc[2].len = 1;
    desc[2].flags = VIRTQ_DESC_F_WRITE;
    desc[2].next = 0;
    asm volatile("" ::: "memory");
    *avail_ring_ptr(next_avail_idx % queue_size) = 0;
    asm volatile("" ::: "memory");
    next_avail_idx = (uint16_t)(next_avail_idx + 1);
    *avail_idx_ptr() = next_avail_idx;
    asm volatile("" ::: "memory");
    hw_outw((uint16_t)(io_base + VIRTIO_REG_QUEUE_NOTIFY), 0);
    if (poll_for_completion() != 0) {
        return -1;
    }
    if (*req_status != VIRTIO_BLK_S_OK) {
        return -1;
    }
    if (!is_write) {
        uint8_t *dst = (uint8_t *)buf;
        const uint8_t *src = (const uint8_t *)req_data;
        for (uint32_t i = 0; i < bytes; i++) {
            dst[i] = src[i];
        }
    }
    return 0;
}

static uint64_t align4096(uint64_t x) {
    return (x + 4095) & ~(uint64_t)4095;
}

static int find_last_virtio_blk(uint8_t *out_bus, uint8_t *out_dev, uint8_t *out_fn) {
    int found = 0;
    for (int bus = 0; bus < 256; bus++) {
        for (int dev = 0; dev < 32; dev++) {
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
                    *out_bus = (uint8_t)bus;
                    *out_dev = (uint8_t)dev;
                    *out_fn = (uint8_t)fn;
                }
            }
        }
    }
    return found;
}

static int virtio_blk_probe_init(void) {
    uint8_t bus = 0, dev = 0, fn = 0;
    if (!find_last_virtio_blk(&bus, &dev, &fn)) {
        return -1;
    }

    uint16_t cmd = (uint16_t)pci_cfg(bus, dev, fn, 0x04, 2, 0, 0);
    pci_cfg(bus, dev, fn, 0x04, 2, 1, cmd | 0x1 | 0x4);
    uint32_t bar0 = (uint32_t)pci_cfg(bus, dev, fn, 0x10, 4, 0, 0);
    if (!(bar0 & 0x1)) {
        return -1;
    }
    io_base = (uint16_t)(bar0 & ~0x3u);

    hw_outb(io_base + VIRTIO_REG_DEVICE_STATUS, 0);
    hw_outb(io_base + VIRTIO_REG_DEVICE_STATUS, VIRTIO_STATUS_ACKNOWLEDGE);
    hw_outb(io_base + VIRTIO_REG_DEVICE_STATUS, VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);

    hw_outl(io_base + VIRTIO_REG_GUEST_FEATURES, 0);
    hw_outw(io_base + VIRTIO_REG_QUEUE_SELECT, 0);
    uint16_t dev_queue_size = hw_inw(io_base + VIRTIO_REG_QUEUE_SIZE);
    if (dev_queue_size == 0 || dev_queue_size > VIRTIO_BLK_MAX_QUEUE_SIZE) {
        hw_outb(io_base + VIRTIO_REG_DEVICE_STATUS, VIRTIO_STATUS_FAILED);
        return -1;
    }
    queue_size = dev_queue_size;
    uint64_t desc_bytes = 16ull * queue_size;
    uint64_t avail_bytes = 6ull + 2ull * queue_size;
    uint64_t used_off = align4096(desc_bytes + avail_bytes);
    uint64_t used_bytes = 6ull + 8ull * queue_size;
    uint64_t queue_total = used_off + align4096(used_bytes);

    uint64_t queue_va, queue_pa;
    uint64_t hdr_va, status_va, data_va;
    if (dma_alloc(queue_total, &queue_va, &queue_pa) != 0 ||
        dma_alloc(sizeof(virtio_blk_req_hdr_t), &hdr_va, &req_hdr_pa) != 0 ||
        dma_alloc(1, &status_va, &req_status_pa) != 0 ||
        dma_alloc(VIRTIO_BLK_DATA_MAX, &data_va, &req_data_pa) != 0) {
        hw_outb(io_base + VIRTIO_REG_DEVICE_STATUS, VIRTIO_STATUS_FAILED);
        return -1;
    }
    desc = (volatile virtq_desc_t *)queue_va;
    avail_base = (volatile uint8_t *)(queue_va + desc_bytes);
    used_base = (volatile uint8_t *)(queue_va + used_off);
    req_hdr = (volatile virtio_blk_req_hdr_t *)hdr_va;
    req_status = (volatile uint8_t *)status_va;
    req_data = (volatile uint8_t *)data_va;
    next_avail_idx = 0;
    last_seen_used_idx = 0;

    hw_outl(io_base + VIRTIO_REG_QUEUE_ADDRESS, (uint32_t)(queue_pa >> 12));
    uint32_t cap_lo = hw_inl(io_base + VIRTIO_REG_CONFIG);
    uint32_t cap_hi = hw_inl((uint16_t)(io_base + VIRTIO_REG_CONFIG + 4));
    capacity_sectors = (uint64_t)cap_lo | ((uint64_t)cap_hi << 32);
    hw_outb(io_base + VIRTIO_REG_DEVICE_STATUS,
            VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_DRIVER_OK);
    present = 1;
    return 0;
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

static int ide_probe_init(void) {
    hw_outb(IDE_PRIMARY_IO_BASE + IDE_REG_DRIVE_HEAD, 0xE0);
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
    ide_capacity_sectors = (uint64_t)identify[60] | ((uint64_t)identify[61] << 16);
    if (ide_capacity_sectors == 0) {
        return -1;
    }
    return 0;
}

static int ide_do_request(uint64_t sector, uint32_t count, void *buf, int is_write) {
    uint8_t *bytes = (uint8_t *)buf;
    for (uint32_t s = 0; s < count; s++) {
        uint64_t lba = sector + s;
        uint8_t status;
        if (ide_wait_status(0, IDE_STATUS_BSY, &status) != 0) {
            return -1;
        }
        hw_outb(IDE_PRIMARY_IO_BASE + IDE_REG_DRIVE_HEAD, (uint8_t)(0xE0 | ((lba >> 24) & 0x0F)));
        hw_outb(IDE_PRIMARY_IO_BASE + IDE_REG_SECCOUNT, 1);
        hw_outb(IDE_PRIMARY_IO_BASE + IDE_REG_LBA_LOW, (uint8_t)(lba & 0xFF));
        hw_outb(IDE_PRIMARY_IO_BASE + IDE_REG_LBA_MID, (uint8_t)((lba >> 8) & 0xFF));
        hw_outb(IDE_PRIMARY_IO_BASE + IDE_REG_LBA_HIGH, (uint8_t)((lba >> 16) & 0xFF));
        hw_outb(IDE_PRIMARY_IO_BASE + IDE_REG_COMMAND,
                is_write ? IDE_CMD_WRITE_SECTORS : IDE_CMD_READ_SECTORS);

        if (ide_wait_status(IDE_STATUS_DRQ, IDE_STATUS_BSY, &status) != 0) {
            return -1;
        }

        uint8_t *sector_buf = bytes + (uint64_t)s * 512;
        if (is_write) {
            for (int i = 0; i < 256; i++) {
                uint16_t w = (uint16_t)sector_buf[i * 2] | ((uint16_t)sector_buf[i * 2 + 1] << 8);
                hw_outw(IDE_PRIMARY_IO_BASE + IDE_REG_DATA, w);
            }
        } else {
            for (int i = 0; i < 256; i++) {
                uint16_t w = hw_inw(IDE_PRIMARY_IO_BASE + IDE_REG_DATA);
                sector_buf[i * 2] = (uint8_t)w;
                sector_buf[i * 2 + 1] = (uint8_t)(w >> 8);
            }
        }

        if (ide_wait_status(0, IDE_STATUS_BSY, &status) != 0) {
            return -1;
        }
    }
    return 0;
}

static int blk_do_request(uint64_t sector, uint32_t count, void *buf, int is_write) {
    if (g_backend == BLK_BACKEND_VIRTIO) {
        return do_request(is_write ? VIRTIO_BLK_T_OUT : VIRTIO_BLK_T_IN, sector, count, buf, is_write);
    }
    if (g_backend == BLK_BACKEND_IDE) {
        return ide_do_request(sector, count, buf, is_write);
    }
    return -1;
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
static void pack_path(uint64_t words[4], const char *path) {
    for (int i = 0; i < 4; i++) {
        words[i] = 0;
    }
    for (int i = 0; path[i] && i < 32; i++) {
        words[i / 8] |= ((uint64_t)(uint8_t)path[i]) << (8 * (i % 8));
    }
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
static void sock_read_bytes(int sockid, uint8_t *buf, int len) {
    int total = 0;
    while (total < len) {
        int n = 0;
        int64_t rc = sock_read_call(sockid, buf + total, len - total, &n);
        if (rc == IPC_ERR_NONE) {
            total += n;
        }
        if (total < len) {
            ipc_sleep(1);
        }
    }
}
static void sock_write_bytes(int sockid, const uint8_t *buf, int len) {
    int total = 0;
    while (total < len) {
        int n = 0;
        if (sock_write_call(sockid, buf + total, len - total, &n) == IPC_ERR_NONE) {
            total += n;
        } else {
            ipc_sleep(1);
        }
    }
}

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

void _start(void) {
    if (virtio_blk_probe_init() == 0) {
        g_backend = BLK_BACKEND_VIRTIO;
    } else if (ide_probe_init() == 0) {
        g_backend = BLK_BACKEND_IDE;
    }
    publish_block_info();

    int sockid = -1;
    sock_create_call(BLOCKDRV_AF_UNIX, BLOCKDRV_SOCK_STREAM, &sockid);
    sock_bind_call(sockid, BLOCKDRV_SOCK_PATH);
    sock_listen_call(sockid, 1);

    int shmid = -1;
    while (shm_get_call(BLOCKDRV_SHM_KEY, BLOCKDRV_BLOCK_SIZE,
                         BLOCKDRV_IPC_CREAT | 0600, &shmid) != IPC_ERR_NONE) {
        ipc_sleep(1);
    }
    uint64_t shm_va = 0;
    while (shm_at_call(shmid, 0, &shm_va) != IPC_ERR_NONE) {
        ipc_sleep(1);
    }
    volatile uint8_t *shm_buf = (volatile uint8_t *)shm_va;

    int connid = -1;
    while (sock_accept_call(sockid, &connid) != IPC_ERR_NONE) {
        ipc_sleep(1);
    }

    for (;;) {
        uint64_t req[3];
        sock_read_bytes(connid, (uint8_t *)req, sizeof(req));
        uint64_t op = req[0];
        uint64_t sector = req[1];
        uint64_t count = req[2];
        if (count == 0 || count > VIRTIO_BLK_DATA_MAX / 512) {
            count = VIRTIO_BLK_DATA_MAX / 512;
        }
        uint32_t bytes = (uint32_t)(count * 512);
        int rc;
        if (op == BLOCKDRV_OP_WRITE) {
            static uint8_t wbuf[VIRTIO_BLK_DATA_MAX];
            for (uint32_t i = 0; i < bytes; i++) {
                wbuf[i] = shm_buf[i];
            }
            rc = blk_do_request(sector, (uint32_t)count, wbuf, 1);
        } else {
            static uint8_t rbuf[VIRTIO_BLK_DATA_MAX];
            rc = blk_do_request(sector, (uint32_t)count, rbuf, 0);
            if (rc == 0) {
                for (uint32_t i = 0; i < bytes; i++) {
                    shm_buf[i] = rbuf[i];
                }
            }
        }
        uint64_t status = rc == 0 ? 0 : (uint64_t)-1;
        sock_write_bytes(connid, (const uint8_t *)&status, sizeof(status));
    }
}
