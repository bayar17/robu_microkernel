#include "ext2.h"

#define JOURNAL_MAGIC0 'R'
#define JOURNAL_MAGIC1 'B'
#define JOURNAL_MAGIC2 'J'
#define JOURNAL_MAGIC3 'N'
#define JOURNAL_MAGIC4 'L'
#define JOURNAL_MAGIC5 '0'
#define JOURNAL_MAGIC6 '0'
#define JOURNAL_MAGIC7 '1'
#define JOURNAL_VERSION 1U
#define JOURNAL_STATE_PREPARED 1U
#define JOURNAL_STATE_COMMITTED 2U
#define JOURNAL_HEADER_SECTOR 0U
#define JOURNAL_DESCRIPTOR_SECTOR 1U
#define JOURNAL_DESCRIPTOR_SECTORS 2U
#define JOURNAL_PAYLOAD_SECTOR 3U
#define JOURNAL_HEADER_CHECKSUM_OFF 24
#define JOURNAL_DESCRIPTOR_CHECKSUM_OFF 28

typedef struct {
    uint64_t sector;
    uint8_t data[512];
} journal_record_t;

static uint32_t g_journal_blocks[EXT2FS_MAX_BLOCKS];
static uint32_t g_journal_block_count;
static int g_journal_ready;
static int g_journal_active;
static int g_journal_faulted;
static uint32_t g_journal_record_count;
static uint64_t g_journal_sequence;
static uint8_t g_journal_sb_snapshot[1024];
static journal_record_t g_journal_records[EXT2FS_JOURNAL_RECORDS];

static uint32_t journal_checksum(const uint8_t *data, uint32_t len) {
    uint32_t value = 2166136261U;
    for (uint32_t i = 0; i < len; i++) {
        value ^= data[i];
        value *= 16777619U;
    }
    return value;
}

static int journal_is_zero(const uint8_t *data, uint32_t len) {
    for (uint32_t i = 0; i < len; i++) {
        if (data[i] != 0) {
            return 0;
        }
    }
    return 1;
}

static int journal_storage_sector(uint64_t sector) {
    uint32_t block = (uint32_t)(sector / g_sectors_per_block);
    for (uint32_t i = 0; i < g_journal_block_count; i++) {
        if (g_journal_blocks[i] == block) {
            return 1;
        }
    }
    return 0;
}

static int journal_map_sector(uint32_t sector, uint64_t *out_sector) {
    uint32_t block_index = sector / g_sectors_per_block;
    uint32_t sector_index = sector % g_sectors_per_block;
    if (block_index >= g_journal_block_count || g_journal_blocks[block_index] == 0) {
        return -1;
    }
    *out_sector = (uint64_t)g_journal_blocks[block_index] * g_sectors_per_block + sector_index;
    return 0;
}

static int journal_disk_read(uint32_t sector, uint8_t out[512]) {
    uint64_t mapped;
    if (journal_map_sector(sector, &mapped) != 0) {
        return -1;
    }
    return blkdev_read(mapped, 1, out);
}

static int journal_disk_write(uint32_t sector, const uint8_t in[512]) {
    uint64_t mapped;
    if (journal_map_sector(sector, &mapped) != 0) {
        return -1;
    }
    return blkdev_write(mapped, 1, in);
}

static void journal_make_header(uint8_t header[512], uint32_t state, uint32_t count) {
    for (uint32_t i = 0; i < 512; i++) {
        header[i] = 0;
    }
    header[0] = JOURNAL_MAGIC0;
    header[1] = JOURNAL_MAGIC1;
    header[2] = JOURNAL_MAGIC2;
    header[3] = JOURNAL_MAGIC3;
    header[4] = JOURNAL_MAGIC4;
    header[5] = JOURNAL_MAGIC5;
    header[6] = JOURNAL_MAGIC6;
    header[7] = JOURNAL_MAGIC7;
    u32_set(header, 8, JOURNAL_VERSION);
    u32_set(header, 12, state);
    u32_set(header, 16, count);
    u32_set(header, 20, (uint32_t)g_journal_sequence);
}

static int journal_header_valid(const uint8_t header[512]) {
    return header[0] == JOURNAL_MAGIC0 && header[1] == JOURNAL_MAGIC1 &&
           header[2] == JOURNAL_MAGIC2 && header[3] == JOURNAL_MAGIC3 &&
           header[4] == JOURNAL_MAGIC4 && header[5] == JOURNAL_MAGIC5 &&
           header[6] == JOURNAL_MAGIC6 && header[7] == JOURNAL_MAGIC7 &&
           u32_get(header, 8) == JOURNAL_VERSION;
}

static int journal_clear(void) {
    uint8_t clear[512];
    for (uint32_t i = 0; i < 512; i++) {
        clear[i] = 0;
    }
    if (journal_disk_write(JOURNAL_HEADER_SECTOR, clear) != 0) {
        return -1;
    }
    return blkdev_flush();
}

static int journal_write_descriptors(void) {
    uint8_t descriptors[JOURNAL_DESCRIPTOR_SECTORS * 512];
    for (uint32_t i = 0; i < sizeof(descriptors); i++) {
        descriptors[i] = 0;
    }
    for (uint32_t i = 0; i < g_journal_record_count; i++) {
        uint32_t off = i * 16;
        uint64_t sector = g_journal_records[i].sector;
        for (uint32_t b = 0; b < 8; b++) {
            descriptors[off + b] = (uint8_t)(sector >> (8 * b));
        }
        u32_set(descriptors, (int)off + 8, journal_checksum(g_journal_records[i].data, 512));
    }
    for (uint32_t i = 0; i < JOURNAL_DESCRIPTOR_SECTORS; i++) {
        if (journal_disk_write(JOURNAL_DESCRIPTOR_SECTOR + i, descriptors + i * 512) != 0) {
            return -1;
        }
    }
    return 0;
}

static int journal_write_payloads(void) {
    for (uint32_t i = 0; i < g_journal_record_count; i++) {
        if (journal_disk_write(JOURNAL_PAYLOAD_SECTOR + i, g_journal_records[i].data) != 0) {
            return -1;
        }
    }
    return 0;
}

static int journal_write_header(uint32_t state) {
    uint8_t header[512];
    journal_make_header(header, state, g_journal_record_count);
    uint8_t descriptors[JOURNAL_DESCRIPTOR_SECTORS * 512];
    for (uint32_t i = 0; i < JOURNAL_DESCRIPTOR_SECTORS; i++) {
        if (journal_disk_read(JOURNAL_DESCRIPTOR_SECTOR + i, descriptors + i * 512) != 0) {
            return -1;
        }
    }
    u32_set(header, JOURNAL_DESCRIPTOR_CHECKSUM_OFF,
            journal_checksum(descriptors, sizeof(descriptors)));
    u32_set(header, JOURNAL_HEADER_CHECKSUM_OFF, 0);
    u32_set(header, JOURNAL_HEADER_CHECKSUM_OFF, journal_checksum(header, 512));
    return journal_disk_write(JOURNAL_HEADER_SECTOR, header);
}

static int journal_replay(void) {
    uint8_t header[512];
    if (journal_disk_read(JOURNAL_HEADER_SECTOR, header) != 0) {
        return -1;
    }
    if (journal_is_zero(header, sizeof(header))) {
        return 0;
    }
    if (!journal_header_valid(header)) {
        return -1;
    }
    uint32_t saved_header_checksum = u32_get(header, JOURNAL_HEADER_CHECKSUM_OFF);
    u32_set(header, JOURNAL_HEADER_CHECKSUM_OFF, 0);
    if (saved_header_checksum != journal_checksum(header, sizeof(header))) {
        return -1;
    }
    uint32_t state = u32_get(header, 12);
    uint32_t count = u32_get(header, 16);
    if ((state != JOURNAL_STATE_PREPARED && state != JOURNAL_STATE_COMMITTED) ||
        count > EXT2FS_JOURNAL_RECORDS) {
        return -1;
    }
    uint8_t descriptors[JOURNAL_DESCRIPTOR_SECTORS * 512];
    for (uint32_t i = 0; i < JOURNAL_DESCRIPTOR_SECTORS; i++) {
        if (journal_disk_read(JOURNAL_DESCRIPTOR_SECTOR + i, descriptors + i * 512) != 0) {
            return -1;
        }
    }
    if (u32_get(header, JOURNAL_DESCRIPTOR_CHECKSUM_OFF) !=
        journal_checksum(descriptors, sizeof(descriptors))) {
        return -1;
    }
    if (state == JOURNAL_STATE_PREPARED) {
        return journal_clear();
    }
    for (uint32_t i = 0; i < count; i++) {
        uint32_t off = i * 16;
        uint64_t sector = 0;
        for (uint32_t b = 0; b < 8; b++) {
            sector |= (uint64_t)descriptors[off + b] << (8 * b);
        }
        if (sector >= (uint64_t)g_blocks_count * g_sectors_per_block ||
            journal_storage_sector(sector)) {
            return -1;
        }
        uint8_t payload[512];
        if (journal_disk_read(JOURNAL_PAYLOAD_SECTOR + i, payload) != 0 ||
            u32_get(descriptors, (int)off + 8) != journal_checksum(payload, sizeof(payload)) ||
            blkdev_write(sector, 1, payload) != 0) {
            return -1;
        }
    }
    if (blkdev_flush() != 0) {
        return -1;
    }
    return journal_clear();
}

static int journal_mount(void) {
    uint32_t journal_ino;
    uint32_t journal_size;
    int journal_is_dir;
    if (find_in_dir(EXT2FS_ROOT_INODE, EXT2FS_JOURNAL_FILE_NAME,
                    sizeof(EXT2FS_JOURNAL_FILE_NAME) - 1,
                    &journal_ino, &journal_size, &journal_is_dir) != 0 || journal_is_dir ||
        journal_size < (uint32_t)EXT2FS_JOURNAL_SECTORS * 512) {
        return 0;
    }
    uint8_t inode_buf[128];
    if (inode_io(journal_ino, inode_buf, 0) != 0 ||
        (u16_get(inode_buf, 0) & 0xF000) != EXT2_S_IFREG) {
        return -1;
    }
    uint32_t needed_blocks = (EXT2FS_JOURNAL_SECTORS + g_sectors_per_block - 1) /
                             g_sectors_per_block;
    if (walk_inode_blocks(inode_buf, needed_blocks, g_journal_blocks, &g_journal_block_count) != 0 ||
        g_journal_block_count != needed_blocks) {
        return -1;
    }
    for (uint32_t i = 0; i < g_journal_block_count; i++) {
        if (g_journal_blocks[i] == 0) {
            return -1;
        }
    }
    g_journal_ready = 1;
    if (journal_replay() != 0 || blkdev_read(2, 2, g_sb) != 0) {
        g_journal_ready = 0;
        return -1;
    }
    return 0;
}

static int journal_begin(void) {
    if (!g_journal_ready || g_journal_active || g_journal_faulted) {
        return -1;
    }
    for (uint32_t i = 0; i < sizeof(g_sb); i++) {
        g_journal_sb_snapshot[i] = g_sb[i];
    }
    g_journal_record_count = 0;
    g_journal_active = 1;
    return 0;
}

static void journal_abort(void) {
    if (!g_journal_active) {
        return;
    }
    for (uint32_t i = 0; i < sizeof(g_sb); i++) {
        g_sb[i] = g_journal_sb_snapshot[i];
    }
    g_journal_record_count = 0;
    g_journal_active = 0;
}

static int journal_commit(void) {
    if (!g_journal_active) {
        return -1;
    }
    if (g_journal_record_count == 0) {
        g_journal_active = 0;
        return 0;
    }
    g_journal_sequence++;
    if (journal_write_descriptors() != 0 || journal_write_payloads() != 0 ||
        journal_write_header(JOURNAL_STATE_PREPARED) != 0 || blkdev_flush() != 0) {
        journal_abort();
        return -1;
    }
    if (journal_write_header(JOURNAL_STATE_COMMITTED) != 0 || blkdev_flush() != 0) {
        g_journal_active = 0;
        g_journal_faulted = 1;
        return -1;
    }
    for (uint32_t i = 0; i < g_journal_record_count; i++) {
        if (blkdev_write(g_journal_records[i].sector, 1, g_journal_records[i].data) != 0) {
            g_journal_active = 0;
            g_journal_faulted = 1;
            return -1;
        }
    }
    if (blkdev_flush() != 0 || journal_clear() != 0) {
        g_journal_active = 0;
        g_journal_faulted = 1;
        return -1;
    }
    g_journal_record_count = 0;
    g_journal_active = 0;
    return 0;
}

static int journal_finish(int result) {
    if (result != 0) {
        journal_abort();
        return -1;
    }
    return journal_commit();
}

static int journal_metadata_read(uint64_t sector, uint32_t count, void *out) {
    if (blkdev_read(sector, count, out) != 0) {
        return -1;
    }
    if (!g_journal_active) {
        return 0;
    }
    uint8_t *bytes = (uint8_t *)out;
    for (uint32_t i = 0; i < g_journal_record_count; i++) {
        if (g_journal_records[i].sector < sector ||
            g_journal_records[i].sector >= sector + count) {
            continue;
        }
        uint32_t offset = (uint32_t)(g_journal_records[i].sector - sector) * 512;
        for (uint32_t b = 0; b < 512; b++) {
            bytes[offset + b] = g_journal_records[i].data[b];
        }
    }
    return 0;
}

static int journal_metadata_write(uint64_t sector, uint32_t count, const void *in) {
    if (!g_journal_active) {
        return blkdev_write(sector, count, in);
    }
    const uint8_t *bytes = (const uint8_t *)in;
    for (uint32_t s = 0; s < count; s++) {
        uint64_t target = sector + s;
        if (journal_storage_sector(target)) {
            return -1;
        }
        uint32_t i;
        for (i = 0; i < g_journal_record_count; i++) {
            if (g_journal_records[i].sector == target) {
                break;
            }
        }
        if (i == g_journal_record_count) {
            if (g_journal_record_count == EXT2FS_JOURNAL_RECORDS) {
                return -1;
            }
            g_journal_records[i].sector = target;
            g_journal_record_count++;
        }
        for (uint32_t b = 0; b < 512; b++) {
            g_journal_records[i].data[b] = bytes[s * 512 + b];
        }
    }
    return 0;
}

static int journal_is_active(void) {
    return g_journal_active;
}

static int journal_is_reserved_name(const char *name, int name_len) {
    if (name_len != (int)(sizeof(EXT2FS_JOURNAL_FILE_NAME) - 1)) {
        return 0;
    }
    for (int i = 0; i < name_len; i++) {
        if (name[i] != EXT2FS_JOURNAL_FILE_NAME[i]) {
            return 0;
        }
    }
    return 1;
}
