#include "robu/kinfo.h"
#include "robu/vm.h"
#include "robu/pmm.h"
#include "robu/sched.h"
#include "robu/arch.h"
#include "robu/random.h"
#include "robu/vfs.h"
#include "robu/linux_vfs.h"
extern paddr_t boot_pml4;
static kinfo_page_t *kinfo;
void kinfo_init(uint32_t boot_apic_id, uint32_t cpu_count) {
    paddr_t frame = pmm_alloc(PMM_COLOR_ANY);
    kinfo = (kinfo_page_t *)frame;
    memset(kinfo, 0, PAGE_SIZE_4K);
    kinfo->abi_version_major = ROBU_ABI_VERSION_MAJOR;
    kinfo->abi_version_minor = ROBU_ABI_VERSION_MINOR;
    kinfo->feature_bits = KINFO_FEATURE_SMP | KINFO_FEATURE_ELF_LOADER |
                          KINFO_FEATURE_ROBU_VFS | KINFO_FEATURE_LINUX_VFS;
    if (random_available()) {
        kinfo->feature_bits |= KINFO_FEATURE_RANDOM;
    }
    kinfo->cpu_count = cpu_count;
    kinfo->boot_apic_id = boot_apic_id;
    kinfo->clock_seq = 0;
    kinfo->clock_ticks = 0;
    kinfo->clock_hz = SCHED_HZ;
    kinfo->vfs_abi_major = ROBU_VFS_ABI_MAJOR;
    kinfo->vfs_abi_minor = ROBU_VFS_ABI_MINOR;
    kinfo->robu_vfs_feature_bits = ROBU_VFS_FEATURE_OPEN |
                                    ROBU_VFS_FEATURE_READ |
                                    ROBU_VFS_FEATURE_WRITE |
                                    ROBU_VFS_FEATURE_STAT |
                                    ROBU_VFS_FEATURE_READDIR |
                                    ROBU_VFS_FEATURE_MUTATE |
                                    ROBU_VFS_FEATURE_SYMLINK |
                                    ROBU_VFS_FEATURE_XATTR |
                                    ROBU_VFS_FEATURE_TIMESTAMPS |
                                    ROBU_VFS_FEATURE_LINUX_VFS;
    kinfo->linux_vfs_feature_bits = robu_linux_vfs_features();
    arch_vm_map_page((paddr_t)&boot_pml4, KINFO_VA, frame, VM_PROT_READ | VM_PROT_USER);
}
void kinfo_set_devfs_tid(uint32_t tid) {
    kinfo->devfs_tid = tid;
}
void kinfo_set_test_report_tid(uint32_t tid) {
    kinfo->test_report_tid = tid;
}
void kinfo_set_benchserver_tid(uint32_t tid) {
    kinfo->benchserver_tid = tid;
}
void kinfo_set_abitest_helper_tid(uint32_t tid) {
    kinfo->abitest_helper_tid = tid;
}
void kinfo_set_abitest_slots(uint32_t untyped, uint32_t notif, uint32_t timer,
                             uint32_t helper_tcb, uint32_t revoke_frame) {
    kinfo->abitest_slots[0] = untyped;
    kinfo->abitest_slots[1] = notif;
    kinfo->abitest_slots[2] = timer;
    kinfo->abitest_slots[3] = helper_tcb;
    kinfo->abitest_slots[4] = revoke_frame;
}
void kinfo_set_ramfs_tid(uint32_t tid) {
    kinfo->ramfs_tid = tid;
}
void kinfo_set_abitest_exit_helper_tid(uint32_t tid) {
    kinfo->abitest_exit_helper_tid = tid;
}
void kinfo_set_procfs_tid(uint32_t tid) {
    kinfo->procfs_tid = tid;
}
void kinfo_set_sysfs_tid(uint32_t tid) {
    kinfo->sysfs_tid = tid;
}
void kinfo_set_blockdrv_tid(uint32_t tid) {
    kinfo->blockdrv_tid = tid;
}
void kinfo_set_ext2fs_tid(uint32_t tid) {
    kinfo->ext2fs_tid = tid;
}
void kinfo_set_diskfs_tid(uint32_t tid) {
    kinfo->diskfs_tid = tid;
}
int kinfo_block_info_publish(uint32_t device, uint32_t transport,
                             uint32_t filesystem, uint32_t sector_size,
                             uint64_t sectors) {
    if (device == 0 || device > BLOCK_DEVICE_MAX ||
        transport == BLOCK_TRANSPORT_NONE || filesystem == BLOCK_FS_NONE ||
        sector_size == 0 || sectors == 0) {
        return -1;
    }
    block_device_info_t *info = &kinfo->block_devices[device - 1];
    kinfo->block_info_seq++;
    asm volatile("" ::: "memory");
    info->device = device;
    info->transport = transport;
    info->filesystem = filesystem;
    info->sector_size = sector_size;
    info->sectors = sectors;
    info->in_use = 1;
    asm volatile("" ::: "memory");
    kinfo->block_info_seq++;
    return 0;
}
int kinfo_block_info_query(uint32_t index, block_device_info_t *out) {
    if (!out || index >= BLOCK_DEVICE_MAX) {
        return -1;
    }
    uint32_t seq0;
    uint32_t seq1;
    do {
        seq0 = kinfo->block_info_seq;
        asm volatile("" ::: "memory");
        *out = kinfo->block_devices[index];
        asm volatile("" ::: "memory");
        seq1 = kinfo->block_info_seq;
    } while (seq0 != seq1 || (seq0 & 1u));
    return out->in_use ? 0 : -1;
}
int kinfo_mount_add(const char *prefix, uint32_t owner_tid) {
    int slot = -1;
    for (int i = 0; i < MOUNT_TABLE_MAX; i++) {
        if (!kinfo->mounts[i].in_use) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        return -1;
    }
    kinfo->mount_seq++;
    asm volatile("" ::: "memory");
    kinfo->mounts[slot].owner_tid = owner_tid;
    int i = 0;
    for (; i < MOUNT_PREFIX_MAX - 1 && prefix[i]; i++) {
        kinfo->mounts[slot].prefix[i] = prefix[i];
    }
    kinfo->mounts[slot].prefix[i] = '\0';
    kinfo->mounts[slot].in_use = 1;
    asm volatile("" ::: "memory");
    kinfo->mount_seq++;
    return 0;
}
void kinfo_tick(uint64_t n) {
    kinfo->clock_seq++;
    asm volatile("" ::: "memory");
    kinfo->clock_ticks += n;
    asm volatile("" ::: "memory");
    kinfo->clock_seq++;
}
