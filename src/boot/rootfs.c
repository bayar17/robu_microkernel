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

void ramfs_extract_tree(void) {
    tar_entry_t entry;
    uint64_t index = 0;
    uint32_t nfiles = 0, nsyms = 0;
    while (tar_iterate_all(rootfs_buf, rootfs_len, index, &entry) == 0) {
        index++;
        int has_slash = 0;
        for (int i = 0; entry.name[i]; i++) {
            if (entry.name[i] == '/') {
                has_slash = 1;
                break;
            }
        }
        if (!has_slash) {
            continue;
        }
        if (entry.type == TAR_ENTRY_SYMLINK) {
            int64_t rc = vfs_symlink(ramfs_tid(), entry.name, entry.linkname);
            if (rc != 0) {
                kprintf("[boot] rootfs extract: FATAL: vfs_symlink('%s' -> '%s') failed rc=%ld\n",
                        entry.name, entry.linkname, rc);
                for (;;) { asm volatile("cli; hlt"); }
            }
            nsyms++;
            continue;
        }
        int64_t h = vfs_open(ramfs_tid(), entry.name, VFS_O_CREAT | VFS_O_TRUNC);
        if (h < 0) {
            kprintf("[boot] rootfs extract: FATAL: vfs_open('%s') failed rc=%ld\n", entry.name, h);
            for (;;) { asm volatile("cli; hlt"); }
        }
        uint64_t off = 0;
        while (off < entry.size) {
            uint64_t chunk = entry.size - off;
            if (chunk > VFS_WRITE_MAX) {
                chunk = VFS_WRITE_MAX;
            }
            int64_t n = vfs_write(ramfs_tid(), (uint64_t)h, entry.data + off, chunk);
            if (n <= 0) {
                kprintf("[boot] rootfs extract: FATAL: vfs_write('%s') failed at off=%lu\n",
                        entry.name, off);
                for (;;) { asm volatile("cli; hlt"); }
            }
            off += (uint64_t)n;
        }
        vfs_close(ramfs_tid(), (uint64_t)h);
        nfiles++;
    }
    kprintf("[boot] rootfs extract: %u files + %u symlinks copied into ramfs\n", nfiles, nsyms);
}
