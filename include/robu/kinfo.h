#ifndef ROBU_KINFO_H
#define ROBU_KINFO_H
#include "robu/types.h"
#include "robu/blockinfo.h"
#define KINFO_VA 0x0000000080000000ULL
#define ROBU_ABI_VERSION_MAJOR 1
#define ROBU_ABI_VERSION_MINOR 2
#define KINFO_FEATURE_SMP        (1ULL << 0)
#define KINFO_FEATURE_ELF_LOADER (1ULL << 1)
#define KINFO_FEATURE_RANDOM     (1ULL << 2)
#define KINFO_FEATURE_ROBU_VFS   (1ULL << 3)
#define KINFO_FEATURE_LINUX_VFS  (1ULL << 4)
#define MOUNT_PREFIX_MAX 24
#define MOUNT_TABLE_MAX  16
typedef struct {
    uint32_t in_use;
    uint32_t owner_tid;
    char prefix[MOUNT_PREFIX_MAX];
} mount_entry_t;
typedef struct {
    uint32_t abi_version_major;
    uint32_t abi_version_minor;
    uint64_t feature_bits;
    uint32_t cpu_count;
    uint32_t boot_apic_id;
    volatile uint32_t clock_seq;
    uint64_t clock_ticks;
    uint32_t clock_hz;
    uint32_t devfs_tid;
    uint32_t test_report_tid;
    uint32_t benchserver_tid;
    uint32_t abitest_helper_tid;
    uint32_t abitest_slots[5];
    uint32_t ramfs_tid;
    uint32_t abitest_exit_helper_tid;
    uint32_t procfs_tid;
    uint32_t sysfs_tid;
    uint32_t blockdrv_tid;
    uint32_t ext2fs_tid;
    mount_entry_t mounts[MOUNT_TABLE_MAX];
    volatile uint32_t mount_seq;
    uint32_t vfs_abi_major;
    uint32_t vfs_abi_minor;
    uint64_t robu_vfs_feature_bits;
    uint64_t linux_vfs_feature_bits;
    block_device_info_t block_devices[BLOCK_DEVICE_MAX];
    volatile uint32_t block_info_seq;
    uint32_t diskfs_tid;
} kinfo_page_t;
static inline uint64_t kinfo_read_ticks(const volatile kinfo_page_t *k) {
    uint32_t seq0, seq1;
    uint64_t ticks;
    do {
        seq0 = k->clock_seq;
        asm volatile("" ::: "memory");
        ticks = k->clock_ticks;
        asm volatile("" ::: "memory");
        seq1 = k->clock_seq;
    } while (seq0 != seq1 || (seq0 & 1u));
    return ticks;
}

static inline uint32_t kinfo_resolve_mount(const volatile kinfo_page_t *k, const char *path,
                                           int *matched_len_out) {
    uint32_t seq0, seq1;
    uint32_t best_tid;
    int best_len;
    do {
        best_tid = 0;
        best_len = -1;
        seq0 = k->mount_seq;
        asm volatile("" ::: "memory");
        for (int i = 0; i < MOUNT_TABLE_MAX; i++) {
            if (!k->mounts[i].in_use) {
                continue;
            }
            int len = 0;
            while (len < MOUNT_PREFIX_MAX && k->mounts[i].prefix[len]) {
                len++;
            }
            int j = 0;
            for (; j < len; j++) {
                if (path[j] != k->mounts[i].prefix[j]) {
                    break;
                }
            }
            if (j == len && len > best_len) {
                best_len = len;
                best_tid = k->mounts[i].owner_tid;
            }
        }
        asm volatile("" ::: "memory");
        seq1 = k->mount_seq;
    } while (seq0 != seq1 || (seq0 & 1u));
    if (matched_len_out) {
        *matched_len_out = best_len < 0 ? 0 : best_len;
    }
    return best_tid;
}
void kinfo_init(uint32_t boot_apic_id, uint32_t cpu_count);
void kinfo_set_devfs_tid(uint32_t tid);
void kinfo_set_test_report_tid(uint32_t tid);
void kinfo_set_benchserver_tid(uint32_t tid);
void kinfo_set_abitest_helper_tid(uint32_t tid);
void kinfo_set_abitest_slots(uint32_t untyped, uint32_t notif, uint32_t timer,
                             uint32_t helper_tcb, uint32_t revoke_frame);
void kinfo_set_ramfs_tid(uint32_t tid);
void kinfo_set_abitest_exit_helper_tid(uint32_t tid);
void kinfo_set_procfs_tid(uint32_t tid);
void kinfo_set_sysfs_tid(uint32_t tid);
void kinfo_set_blockdrv_tid(uint32_t tid);
void kinfo_set_ext2fs_tid(uint32_t tid);
void kinfo_set_diskfs_tid(uint32_t tid);

int kinfo_mount_add(const char *prefix, uint32_t owner_tid);
static inline const kinfo_page_t *kinfo_user(void) {
    return (const kinfo_page_t *)KINFO_VA;
}
void kinfo_tick(uint64_t n);
#endif
