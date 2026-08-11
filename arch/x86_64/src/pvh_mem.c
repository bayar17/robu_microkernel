#include "robu/types.h"
#include "robu/arch.h"
#include "robu/kprintf.h"

#define PVH_MAGIC 0x336ec578u
#define MB2_MAGIC 0x36d76289u

struct hvm_start_info {
    uint32_t magic;
    uint32_t version;
    uint32_t flags;
    uint32_t nr_modules;
    uint64_t modlist_paddr;
    uint64_t cmdline_paddr;
    uint64_t rsdp_paddr;
    uint64_t memmap_paddr;
    uint32_t memmap_entries;
    uint32_t reserved;
} __attribute__((packed));

struct hvm_memmap_table_entry {
    uint64_t addr;
    uint64_t size;
    uint32_t type;
    uint32_t reserved;
} __attribute__((packed));

struct hvm_modlist_entry {
    uint64_t paddr;
    uint64_t size;
    uint64_t cmdline_paddr;
    uint64_t reserved;
} __attribute__((packed));

struct mb2_tag {
    uint32_t type;
    uint32_t size;
};

struct mb2_tag_string {
    uint32_t type;
    uint32_t size;
    char string[1];
};

struct mb2_tag_module {
    uint32_t type;
    uint32_t size;
    uint32_t mod_start;
    uint32_t mod_end;
    char cmdline[1];
};

struct mb2_mmap_entry {
    uint64_t addr;
    uint64_t len;
    uint32_t type;
    uint32_t zero;
};

struct mb2_tag_mmap {
    uint32_t type;
    uint32_t size;
    uint32_t entry_size;
    uint32_t entry_version;
    struct mb2_mmap_entry entries[1];
};

struct mb2_tag_rsdp {
    uint32_t type;
    uint32_t size;
    uint8_t rsdp_data[1];
};

#define MB2_TAG_RSDP_OLD 14
#define MB2_TAG_RSDP_NEW 15

#define HVM_MEMMAP_TYPE_RAM 1
extern uint64_t pvh_start_info_ptr;
extern uint64_t multiboot2_info_ptr;

#define FALLBACK_BASE 0x1000000ULL
#define FALLBACK_LEN  0x1000000ULL

static const struct hvm_start_info *pvh_info(void) {
    const struct hvm_start_info *info =
        (const struct hvm_start_info *)pvh_start_info_ptr;
    if (pvh_start_info_ptr && info->magic == PVH_MAGIC && info->version >= 1) {
        return info;
    }
    return NULL;
}

int arch_boot_magic(uint32_t *out_magic) {
    if (multiboot2_info_ptr) {
        *out_magic = MB2_MAGIC;
        return 0;
    }
    const struct hvm_start_info *info = pvh_info();
    if (!info) {
        return -1;
    }
    *out_magic = info->magic;
    return 0;
}

const char *arch_boot_cmdline(void) {
    if (multiboot2_info_ptr) {
        uint32_t total_size = *(uint32_t *)multiboot2_info_ptr;
        uint8_t *ptr = (uint8_t *)(multiboot2_info_ptr + 8);
        while (ptr < (uint8_t *)multiboot2_info_ptr + total_size) {
            struct mb2_tag *tag = (struct mb2_tag *)ptr;
            if (tag->type == 0) break;
            if (tag->type == 1) {
                return ((struct mb2_tag_string *)tag)->string;
            }
            ptr += (tag->size + 7) & ~7;
        }
    }
    const struct hvm_start_info *info = pvh_info();
    if (!info || !info->cmdline_paddr) {
        return NULL;
    }
    return (const char *)info->cmdline_paddr;
}

int arch_boot_module(paddr_t *out_base, uint64_t *out_len) {
    if (multiboot2_info_ptr) {
        uint32_t total_size = *(uint32_t *)multiboot2_info_ptr;
        uint8_t *ptr = (uint8_t *)(multiboot2_info_ptr + 8);
        while (ptr < (uint8_t *)multiboot2_info_ptr + total_size) {
            struct mb2_tag *tag = (struct mb2_tag *)ptr;
            if (tag->type == 0) break;
            if (tag->type == 3) {
                struct mb2_tag_module *mod = (struct mb2_tag_module *)tag;
                *out_base = (paddr_t)mod->mod_start;
                *out_len = (uint64_t)(mod->mod_end - mod->mod_start);
                return 0;
            }
            ptr += (tag->size + 7) & ~7;
        }
    }
    const struct hvm_start_info *info = pvh_info();
    if (!info || info->nr_modules == 0 || !info->modlist_paddr) {
        return -1;
    }
    const struct hvm_modlist_entry *mods =
        (const struct hvm_modlist_entry *)info->modlist_paddr;
    *out_base = (paddr_t)mods[0].paddr;
    *out_len = mods[0].size;
    return 0;
}

int arch_boot_rsdp(paddr_t *out_rsdp) {
    if (multiboot2_info_ptr) {
        uint32_t total_size = *(uint32_t *)multiboot2_info_ptr;
        uint8_t *ptr = (uint8_t *)(multiboot2_info_ptr + 8);
        while (ptr < (uint8_t *)multiboot2_info_ptr + total_size) {
            struct mb2_tag *tag = (struct mb2_tag *)ptr;
            if (tag->type == 0) break;
            if (tag->type == MB2_TAG_RSDP_OLD || tag->type == MB2_TAG_RSDP_NEW) {
                struct mb2_tag_rsdp *rsdp = (struct mb2_tag_rsdp *)tag;
                *out_rsdp = (paddr_t)(uint64_t)&rsdp->rsdp_data[0];
                return 0;
            }
            ptr += (tag->size + 7) & ~7;
        }
    }
    const struct hvm_start_info *info = pvh_info();
    if (info && info->rsdp_paddr) {
        *out_rsdp = (paddr_t)info->rsdp_paddr;
        return 0;
    }
    return -1;
}

void arch_detect_memory(paddr_t *out_base, uint64_t *out_len) {
    if (multiboot2_info_ptr) {
        uint32_t total_size = *(uint32_t *)multiboot2_info_ptr;
        uint8_t *ptr = (uint8_t *)(multiboot2_info_ptr + 8);
        while (ptr < (uint8_t *)multiboot2_info_ptr + total_size) {
            struct mb2_tag *tag = (struct mb2_tag *)ptr;
            if (tag->type == 0) break;
            if (tag->type == 6) {
                struct mb2_tag_mmap *mmap = (struct mb2_tag_mmap *)tag;
                uint32_t nentries = (mmap->size - 16) / mmap->entry_size;
                paddr_t best_base = 0;
                uint64_t best_len = 0;
                for (uint32_t i = 0; i < nentries; i++) {
                    struct mb2_mmap_entry *e = (struct mb2_mmap_entry *)((uint8_t *)mmap->entries + i * mmap->entry_size);

                    if (e->type == 1 && e->len > best_len) {
                        best_base = (paddr_t)e->addr;
                        best_len = e->len;
                    }
                }
                if (best_len > 0) {
                    kprintf("[mem] Multiboot2 memmap: largest RAM region [0x%lx-0x%lx)\n",
                            best_base, best_base + best_len);
                    *out_base = best_base;
                    *out_len = best_len;
                    return;
                }
            }
            ptr += (tag->size + 7) & ~7;
        }
    }

    const struct hvm_start_info *info = pvh_info();
    if (info && info->memmap_paddr && info->memmap_entries > 0) {
        const struct hvm_memmap_table_entry *map =
            (const struct hvm_memmap_table_entry *)info->memmap_paddr;
        paddr_t best_base = 0;
        uint64_t best_len = 0;
        for (uint32_t i = 0; i < info->memmap_entries; i++) {
            if (map[i].type == HVM_MEMMAP_TYPE_RAM && map[i].size > best_len) {
                best_base = map[i].addr;
                best_len = map[i].size;
            }
        }
        if (best_len > 0) {
            kprintf("[mem] PVH memmap: %u entries, largest RAM region [0x%lx-0x%lx)\n",
                    info->memmap_entries, best_base, best_base + best_len);
            *out_base = best_base;
            *out_len = best_len;
            return;
        }
    }

    kprintf("[mem] fallback memory window [0x%lx-0x%lx)\n",
            (uint64_t)FALLBACK_BASE, (uint64_t)(FALLBACK_BASE + FALLBACK_LEN));
    *out_base = FALLBACK_BASE;
    *out_len = FALLBACK_LEN;
}
