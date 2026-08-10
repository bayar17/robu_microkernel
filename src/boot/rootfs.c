#include "robu/types.h"
#include "robu/arch.h"
#include "robu/kprintf.h"
#include "robu/sched.h"
#include "robu/elf.h"
#include "robu/cmdline.h"
#include "robu/tar.h"
#include "robu/captable.h"
#include "robu/vfs.h"
#include "robu/kinfo.h"
#include "robu/rootfs.h"
#include "robu/vm.h"
#include "../boot.h"
extern paddr_t boot_pml4;

static tid_t ramfs_tid(void) {
    return (tid_t)kinfo_user()->ramfs_tid;
}

#define ROOTFS_MAX_SIZE (24 * 1024 * 1024)

static uint8_t rootfs_buf[ROOTFS_MAX_SIZE];
static uint64_t rootfs_len;

int rootfs_lookup(const char *name, const uint8_t **out_start, const uint8_t **out_end) {
    const uint8_t *start;
    uint64_t sz;
    if (tar_find(rootfs_buf, rootfs_len, name, &start, &sz) != 0) {
        return -1;
    }
    *out_start = start;
    *out_end = start + sz;
    return 0;
}

int rootfs_readdir(uint64_t index, char *name_out, uint64_t name_max, uint64_t *out_size) {
    return tar_iterate(rootfs_buf, rootfs_len, index, name_out, name_max, out_size);
}

void rootfs_init(void) {
    cmdline_parse(arch_boot_cmdline());
}

void rootfs_load_module(void) {
    paddr_t mod_base;
    uint64_t mod_len;
    if (arch_boot_module(&mod_base, &mod_len) != 0) {
        kprintf("[boot] FATAL: no boot module supplied -- pass -initrd <rootfs.tar>\n");
        for (;;) { asm volatile("cli; hlt"); }
    }
    if (mod_len > sizeof(rootfs_buf)) {
        kprintf("[boot] FATAL: rootfs module is %lu bytes, exceeds the %lu byte buffer\n",
                mod_len, (uint64_t)sizeof(rootfs_buf));
        for (;;) { asm volatile("cli; hlt"); }
    }
    vaddr_t page = mod_base & ~(PAGE_SIZE_4K - 1);
    vaddr_t end = (mod_base + mod_len + PAGE_SIZE_4K - 1) & ~(PAGE_SIZE_4K - 1);
    for (; page < end; page += PAGE_SIZE_4K) {

        if (arch_vm_translate((paddr_t)&boot_pml4, page) != 0) {
            continue;
        }
        arch_vm_map_page((paddr_t)&boot_pml4, page, (paddr_t)page,
                         VM_PROT_READ | VM_PROT_WRITE);
    }
    memcpy(rootfs_buf, (const void *)mod_base, mod_len);
    rootfs_len = mod_len;
    kprintf("[boot] rootfs: %lu bytes copied from module at 0x%lx\n", mod_len, (uint64_t)mod_base);
}

void root_task_init(paddr_t untyped_base, uint64_t untyped_size) {
    const char *root_name = cmdline_get("root");
    if (!root_name) {
        root_name = "root_task";
    }
    const uint8_t *root_elf_start, *root_elf_end;
    if (rootfs_lookup(root_name, &root_elf_start, &root_elf_end) != 0) {
        kprintf("[boot] FATAL: rootfs has no entry named '%s'\n", root_name);
        for (;;) { asm volatile("cli; hlt"); }
    }
    tcb_t *root = elf_load_and_spawn("root-task", root_elf_start, root_elf_end, 13, PAGER_TID);
    if (!root) {
        kprintf("[boot] FATAL: root task failed to load\n");
        for (;;) { asm volatile("cli; hlt"); }
    }
    kcap_grant(root->tid, CAP_KIND_UNTYPED, (uint64_t)untyped_base, untyped_size);
    kprintf("[boot] root task: tid=%u\n", root->tid);
}

static void seed_binary_copy(const char *rootfs_name, const char *ramfs_path) {
    const uint8_t *start, *end;
    if (rootfs_lookup(rootfs_name, &start, &end) != 0) {
        kprintf("[boot] seed: FATAL: rootfs has no entry named '%s'\n", rootfs_name);
        for (;;) { asm volatile("cli; hlt"); }
    }
    uint64_t sz = (uint64_t)(end - start);
    int64_t h = vfs_open(ramfs_tid(), ramfs_path, VFS_O_CREAT | VFS_O_TRUNC);
    if (h < 0) {
        kprintf("[boot] seed: FATAL: vfs_open('%s') failed rc=%ld\n", ramfs_path, h);
        for (;;) { asm volatile("cli; hlt"); }
    }
    uint64_t off = 0;
    while (off < sz) {
        uint64_t chunk = sz - off;
        if (chunk > VFS_WRITE_MAX) {
            chunk = VFS_WRITE_MAX;
        }
        int64_t n = vfs_write(ramfs_tid(), (uint64_t)h, start + off, chunk);
        if (n <= 0) {
            kprintf("[boot] seed: FATAL: vfs_write('%s') failed at off=%lu\n",
                    ramfs_path, off);
            for (;;) { asm volatile("cli; hlt"); }
        }
        off += (uint64_t)n;
    }
    vfs_close(ramfs_tid(), (uint64_t)h);
}

void ramfs_bin_seed_init(void) {
    static const char *const real_names[] = {
        "sh", "minibox", "file", "find", "mv", "am", "top"
    };
    for (unsigned i = 0; i < sizeof(real_names) / sizeof(real_names[0]); i++) {
        char path[VFS_NAME_MAX] = "bin/";
        for (uint32_t j = 0, p = 4; real_names[i][j] && p < sizeof(path) - 1; j++, p++) {
            path[p] = real_names[i][j];
        }
        seed_binary_copy(real_names[i], path);
    }

    static const char *const minibox_cmds[] = {
        "basename", "cal", "cat", "clear", "cmp", "cp", "cut", "date",
        "dirname", "echo", "env", "expand", "factor", "false", "fold",
        "free", "grep", "head", "hexdump", "hostname", "kill", "link",
        "ls", "mkdir", "mknod", "nohup", "od", "paste", "ps", "rm",
        "rmdir", "sleep", "sort", "sync", "tail", "touch", "tr", "true",
        "tty", "unexpand", "uniq", "unlink", "update", "uptime", "vmstat",
        "w", "wc", "whoami", "xxd", "yes"
    };
    unsigned nsyms = sizeof(minibox_cmds) / sizeof(minibox_cmds[0]);
    for (unsigned i = 0; i < nsyms; i++) {
        char path[VFS_NAME_MAX] = "bin/";
        for (uint32_t j = 0, p = 4; minibox_cmds[i][j] && p < sizeof(path) - 1; j++, p++) {
            path[p] = minibox_cmds[i][j];
        }
        int64_t rc = vfs_symlink(ramfs_tid(), path, "bin/minibox");
        if (rc != 0) {
            kprintf("[boot] /bin seed: FATAL: vfs_symlink('%s') failed rc=%ld\n", path, rc);
            for (;;) { asm volatile("cli; hlt"); }
        }
    }
    kprintf("[boot] /bin seed: %lu real binaries + %u symlinks copied into ramfs\n",
            (uint64_t)(sizeof(real_names) / sizeof(real_names[0])), nsyms);
}

void ramfs_usr_seed_init(void) {
    seed_binary_copy("hello_initsys", "usr/sbin/hello_initsys");
    kprintf("[boot] /usr seed: hello_initsys copied into ramfs\n");
}

void ramfs_sbin_seed_init(void) {
    seed_binary_copy("reboot", "sbin/reboot");
    seed_binary_copy("halt", "sbin/halt");
    seed_binary_copy("shutdown", "sbin/shutdown");
    kprintf("[boot] /sbin seed: reboot, halt, shutdown copied into ramfs\n");
}

static void seed_etc_file(const char *rootfs_name, const char *ramfs_path) {
    const uint8_t *start, *end;
    if (rootfs_lookup(rootfs_name, &start, &end) != 0) {
        kprintf("[boot] /etc seed: no '%s' entry in rootfs, skipping\n", rootfs_name);
        return;
    }
    uint64_t sz = (uint64_t)(end - start);

    int64_t h = vfs_open(ramfs_tid(), ramfs_path, VFS_O_CREAT | VFS_O_TRUNC);
    if (h < 0) {
        kprintf("[boot] /etc seed: vfs_open('%s') failed rc=%ld\n", ramfs_path, h);
        return;
    }
    uint64_t off = 0;
    while (off < sz) {
        uint64_t chunk = sz - off;
        if (chunk > VFS_WRITE_MAX) {
            chunk = VFS_WRITE_MAX;
        }
        int64_t n = vfs_write(ramfs_tid(), (uint64_t)h, start + off, chunk);
        if (n <= 0) {
            kprintf("[boot] /etc seed: vfs_write('%s') failed at off=%lu\n", ramfs_path, off);
            vfs_close(ramfs_tid(), (uint64_t)h);
            return;
        }
        off += (uint64_t)n;
    }
    vfs_close(ramfs_tid(), (uint64_t)h);
    kprintf("[boot] /etc seed: %s (%lu bytes) copied into ramfs\n", ramfs_path, sz);
}

void ramfs_etc_seed_init(void) {
    seed_etc_file("rc.conf", "etc/rc.conf");
    seed_etc_file("passwd", "etc/passwd");
}
