#include "robu/types.h"
#include "robu/arch.h"
#include "robu/kprintf.h"
#include "robu/sched.h"
#include "robu/elf.h"
#include "robu/cmdline.h"
#include "robu/captable.h"
#include "robu/rootfs.h"
#include "../boot.h"

int rootfs_lookup(const char *name, const uint8_t **out_start, const uint8_t **out_end) {
    paddr_t mod_base;
    uint64_t mod_len;
    if (arch_boot_module_by_name(name, &mod_base, &mod_len) == 0) {
        *out_start = (const uint8_t *)mod_base;
        *out_end = (const uint8_t *)mod_base + mod_len;
        return 0;
    }
    return -1;
}
int rootfs_readdir(uint64_t index, char *name_out, uint64_t name_max, uint64_t *out_size) {
    (void)index;
    (void)name_out;
    (void)name_max;
    (void)out_size;
    return -1;
}

void rootfs_init(void) {
    cmdline_parse(arch_boot_cmdline());
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
