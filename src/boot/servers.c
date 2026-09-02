#include "robu/types.h"
#include "robu/kprintf.h"
#include "robu/sched.h"
#include "robu/elf.h"
#include "robu/cmdline.h"
#include "robu/ipc.h"
#include "robu/kinfo.h"
#include "robu/rootfs.h"
#include "robu/hwdrv.h"
#include "../boot.h"

tid_t pager_init(void) {
    const uint8_t *pager_elf_start, *pager_elf_end;
    if (rootfs_lookup("pager", &pager_elf_start, &pager_elf_end) != 0) {
        kprintf("[boot] FATAL: rootfs has no entry named 'pager'\n");
        for (;;) { asm volatile("cli; hlt"); }
    }

    tcb_t *pager = elf_load_and_spawn("pager", pager_elf_start, pager_elf_end, 18, PAGER_TID);
    if (!pager) {
        kprintf("[boot] FATAL: pager failed to load\n");
        for (;;) { asm volatile("cli; hlt"); }
    }

    ipc_grant_pager_owner(pager->tid);
    kprintf("[boot] pager: tid=%u\n", pager->tid);
    return pager->tid;
}

tid_t devfs_init(void) {
    const char *devfs_name = cmdline_get("devfs");
    if (!devfs_name) {
        devfs_name = "devfs";
    }
    const uint8_t *devfs_elf_start, *devfs_elf_end;
    if (rootfs_lookup(devfs_name, &devfs_elf_start, &devfs_elf_end) != 0) {
        kprintf("[boot] FATAL: rootfs has no entry named '%s'\n", devfs_name);
        for (;;) { asm volatile("cli; hlt"); }
    }
    tcb_t *devfs = elf_load_and_spawn("devfs", devfs_elf_start, devfs_elf_end, 11, PAGER_TID);
    if (!devfs) {
        kprintf("[boot] FATAL: devfs server failed to load\n");
        for (;;) { asm volatile("cli; hlt"); }
    }
    ipc_grant_console_writer(devfs->tid);
    kinfo_set_devfs_tid(devfs->tid);
    kinfo_mount_add("/dev/", devfs->tid);
    kprintf("[boot] devfs server: tid=%u, granted console-write permission\n", devfs->tid);
    return devfs->tid;
}

tid_t console_driver_init(void) {
    const uint8_t *cd_elf_start, *cd_elf_end;
    if (rootfs_lookup("console_driver", &cd_elf_start, &cd_elf_end) != 0) {
        kprintf("[boot] FATAL: rootfs has no entry named 'console_driver'\n");
        for (;;) { asm volatile("cli; hlt"); }
    }
    tcb_t *cd = elf_load_and_spawn("console_driver", cd_elf_start, cd_elf_end, 20, PAGER_TID);
    if (!cd) {
        kprintf("[boot] FATAL: console_driver server failed to load\n");
        for (;;) { asm volatile("cli; hlt"); }
    }
    ipc_grant_console_driver(cd->tid);
    kprintf("[boot] console_driver server: tid=%u, granted port-io/console-driver permission\n", cd->tid);
    return cd->tid;
}

tid_t blockdrv_init(void) {
    const uint8_t *bd_elf_start, *bd_elf_end;
    if (rootfs_lookup("blockdrv", &bd_elf_start, &bd_elf_end) != 0) {
        kprintf("[boot] FATAL: rootfs has no entry named 'blockdrv'\n");
        for (;;) { asm volatile("cli; hlt"); }
    }
    tcb_t *bd = elf_load_and_spawn("blockdrv", bd_elf_start, bd_elf_end, 11, PAGER_TID);
    if (!bd) {
        kprintf("[boot] FATAL: blockdrv server failed to load\n");
        for (;;) { asm volatile("cli; hlt"); }
    }
    ipc_grant_hw_driver(bd->tid);
    kinfo_set_blockdrv_tid(bd->tid);
    kprintf("[boot] blockdrv server: tid=%u, granted hw-driver permission\n", bd->tid);
    return bd->tid;
}

tid_t ramfs_init(void) {
    const uint8_t *ramfs_elf_start, *ramfs_elf_end;
    if (rootfs_lookup("ramfs", &ramfs_elf_start, &ramfs_elf_end) != 0) {
        kprintf("[boot] FATAL: rootfs has no entry named 'ramfs'\n");
        for (;;) { asm volatile("cli; hlt"); }
    }
    tcb_t *ramfs = elf_load_and_spawn("ramfs", ramfs_elf_start, ramfs_elf_end, 11, PAGER_TID);
    if (!ramfs) {
        kprintf("[boot] FATAL: ramfs server failed to load\n");
        for (;;) { asm volatile("cli; hlt"); }
    }
    kinfo_set_ramfs_tid(ramfs->tid);

    kinfo_mount_add("/", ramfs->tid);
    kprintf("[boot] ramfs server: tid=%u\n", ramfs->tid);
    return ramfs->tid;
}

tid_t procfs_init(void) {
    const uint8_t *procfs_elf_start, *procfs_elf_end;
    if (rootfs_lookup("procfs", &procfs_elf_start, &procfs_elf_end) != 0) {
        kprintf("[boot] FATAL: rootfs has no entry named 'procfs'\n");
        for (;;) { asm volatile("cli; hlt"); }
    }
    tcb_t *procfs = elf_load_and_spawn("procfs", procfs_elf_start, procfs_elf_end, 11, PAGER_TID);
    if (!procfs) {
        kprintf("[boot] FATAL: procfs server failed to load\n");
        for (;;) { asm volatile("cli; hlt"); }
    }
    kinfo_set_procfs_tid(procfs->tid);
    kinfo_mount_add("/proc/", procfs->tid);
    kprintf("[boot] procfs server: tid=%u\n", procfs->tid);
    return procfs->tid;
}

tid_t sysfs_init(void) {
    const uint8_t *sysfs_elf_start, *sysfs_elf_end;
    if (rootfs_lookup("sysfs", &sysfs_elf_start, &sysfs_elf_end) != 0) {
        kprintf("[boot] FATAL: rootfs has no entry named 'sysfs'\n");
        for (;;) { asm volatile("cli; hlt"); }
    }
    tcb_t *sysfs = elf_load_and_spawn("sysfs", sysfs_elf_start, sysfs_elf_end, 11, PAGER_TID);
    if (!sysfs) {
        kprintf("[boot] FATAL: sysfs server failed to load\n");
        for (;;) { asm volatile("cli; hlt"); }
    }
    kinfo_set_sysfs_tid(sysfs->tid);
    kinfo_mount_add("/var/sys/", sysfs->tid);
    kprintf("[boot] sysfs server: tid=%u\n", sysfs->tid);
    return sysfs->tid;
}

tid_t bootfs_init(void) {
    const uint8_t *bootfs_elf_start, *bootfs_elf_end;
    if (rootfs_lookup("bootfs", &bootfs_elf_start, &bootfs_elf_end) != 0) {
        kprintf("[boot] FATAL: rootfs has no entry named 'bootfs'\n");
        for (;;) { asm volatile("cli; hlt"); }
    }
    tcb_t *bootfs = elf_load_and_spawn("bootfs", bootfs_elf_start, bootfs_elf_end, 11, PAGER_TID);
    if (!bootfs) {
        kprintf("[boot] FATAL: bootfs server failed to load\n");
        for (;;) { asm volatile("cli; hlt"); }
    }
    kinfo_mount_add("/boot/", bootfs->tid);
    kprintf("[boot] bootfs server: tid=%u\n", bootfs->tid);
    return bootfs->tid;
}

tid_t diskfs_init(void) {
    const uint8_t *diskfs_elf_start, *diskfs_elf_end;
    if (rootfs_lookup("diskfs", &diskfs_elf_start, &diskfs_elf_end) != 0) {
        kprintf("[boot] FATAL: rootfs has no entry named 'diskfs'\n");
        for (;;) { asm volatile("cli; hlt"); }
    }
    tcb_t *diskfs = elf_load_and_spawn("diskfs", diskfs_elf_start, diskfs_elf_end, 11, PAGER_TID);
    if (!diskfs) {
        kprintf("[boot] FATAL: diskfs server failed to load\n");
        for (;;) { asm volatile("cli; hlt"); }
    }

    kinfo_set_diskfs_tid(diskfs->tid);
    kinfo_mount_add("/mnt/disk0/", diskfs->tid);
    kprintf("[boot] diskfs server: tid=%u\n", diskfs->tid);
    return diskfs->tid;
}

tid_t ext2fs_init(void) {
    const uint8_t *ext2fs_elf_start, *ext2fs_elf_end;
    if (rootfs_lookup("ext2fsroot", &ext2fs_elf_start, &ext2fs_elf_end) != 0) {
        kprintf("[boot] FATAL: rootfs has no entry named 'ext2fsroot'\n");
        for (;;) { asm volatile("cli; hlt"); }
    }
    tcb_t *ext2fs = elf_load_and_spawn("ext2fsroot", ext2fs_elf_start, ext2fs_elf_end, 11, PAGER_TID);
    if (!ext2fs) {
        kprintf("[boot] FATAL: ext2fs server failed to load\n");
        for (;;) { asm volatile("cli; hlt"); }
    }
    ipc_grant_hw_driver_secondary(ext2fs->tid);
    kinfo_set_ext2fs_tid(ext2fs->tid);
    kinfo_mount_add("/", ext2fs->tid);
    kprintf("[boot] ext2fs server: tid=%u\n", ext2fs->tid);
    return ext2fs->tid;
}

void bench_init(void) {
    if (!cmdline_get("bench")) {
        return;
    }
    const uint8_t *server_elf_start, *server_elf_end;
    const uint8_t *client_elf_start, *client_elf_end;
    if (rootfs_lookup("benchserver", &server_elf_start, &server_elf_end) != 0) {
        kprintf("[boot] FATAL: rootfs has no entry named 'benchserver'\n");
        for (;;) { asm volatile("cli; hlt"); }
    }
    if (rootfs_lookup("benchclient", &client_elf_start, &client_elf_end) != 0) {
        kprintf("[boot] FATAL: rootfs has no entry named 'benchclient'\n");
        for (;;) { asm volatile("cli; hlt"); }
    }
    tcb_t *server = elf_load_and_spawn("bench-server", server_elf_start, server_elf_end, 10, PAGER_TID);
    tcb_t *client = elf_load_and_spawn("bench-client", client_elf_start, client_elf_end, 10, PAGER_TID);
    if (!server || !client) {
        kprintf("[boot] FATAL: bench client/server failed to load\n");
        for (;;) { asm volatile("cli; hlt"); }
    }
    kinfo_set_benchserver_tid(server->tid);
    kprintf("[boot] bench: server tid=%u, client tid=%u\n", server->tid, client->tid);
}
