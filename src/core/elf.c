#include "robu/elf.h"
#include "robu/vm.h"
#include "robu/pmm.h"
#include "robu/sched.h"
#include "robu/kprintf.h"
#include "robu/spinlock.h"
#include "robu/arch.h"
#include "robu/spawn.h"
#include "robu/pipe.h"
#define ELF_CACHE_SIZE 8
#define APP_STACK_PAGES 8
static elf_image_t elf_cache[ELF_CACHE_SIZE];
static uint32_t elf_cache_next_victim;
static spinlock_t elf_lock = SPINLOCK_INIT;
elf_cache_stats_t elf_cache_stats;
static const elf_image_t *cache_lookup(const uint8_t *elf_start) {
    for (int i = 0; i < ELF_CACHE_SIZE; i++) {
        if (elf_cache[i].in_use && elf_cache[i].elf_start == elf_start) {
            return &elf_cache[i];
        }
    }
    return NULL;
}
static uint32_t prot_from_pflags(uint32_t p_flags) {
    uint32_t prot = VM_PROT_USER;
    if (p_flags & PF_R) prot |= VM_PROT_READ;
    if (p_flags & PF_W) prot |= VM_PROT_WRITE;
    if (p_flags & PF_X) prot |= VM_PROT_EXEC;
    return prot;
}
const elf_image_t *elf_parse(const uint8_t *elf_start, const uint8_t *elf_end) {
    spin_lock(&elf_lock);
    const elf_image_t *cached = cache_lookup(elf_start);
    if (cached) {
        elf_cache_stats.cache_hits++;
        spin_unlock(&elf_lock);
        return cached;
    }
    uint64_t blob_len = (uint64_t)(elf_end - elf_start);
    if (blob_len < sizeof(elf64_ehdr_t)) {
        kprintf("[elf] image too small to hold an ELF header\n");
        spin_unlock(&elf_lock);
        return NULL;
    }
    const elf64_ehdr_t *eh = (const elf64_ehdr_t *)elf_start;
    if (eh->e_ident[0] != ELF_MAGIC0 || eh->e_ident[1] != ELF_MAGIC1 ||
        eh->e_ident[2] != ELF_MAGIC2 || eh->e_ident[3] != ELF_MAGIC3) {
        kprintf("[elf] bad magic\n");
        spin_unlock(&elf_lock);
        return NULL;
    }
    if (eh->e_ident[4] != ELFCLASS64 || eh->e_ident[5] != ELFDATA2LSB ||
        eh->e_ident[6] != EV_CURRENT) {
        kprintf("[elf] not a little-endian ELF64 file\n");
        spin_unlock(&elf_lock);
        return NULL;
    }
    if (eh->e_machine != EM_X86_64) {
        kprintf("[elf] wrong machine (e_machine=%u, want %u)\n", eh->e_machine, EM_X86_64);
        spin_unlock(&elf_lock);
        return NULL;
    }
    if (eh->e_type == ET_DYN) {
        kprintf("[elf] PIE (ET_DYN) not supported -- Stage 2D scope is "
                "static non-PIE (ET_EXEC) only\n");
        spin_unlock(&elf_lock);
        return NULL;
    }
    if (eh->e_type != ET_EXEC) {
        kprintf("[elf] unsupported e_type=%u (want ET_EXEC=%u)\n", eh->e_type, ET_EXEC);
        spin_unlock(&elf_lock);
        return NULL;
    }
    if (eh->e_phentsize != sizeof(elf64_phdr_t)) {
        kprintf("[elf] unexpected e_phentsize=%u\n", eh->e_phentsize);
        spin_unlock(&elf_lock);
        return NULL;
    }
    uint64_t phtable_end = eh->e_phoff + (uint64_t)eh->e_phnum * eh->e_phentsize;
    if (eh->e_phoff > blob_len || phtable_end > blob_len || phtable_end < eh->e_phoff) {
        kprintf("[elf] program header table out of bounds\n");
        spin_unlock(&elf_lock);
        return NULL;
    }
    elf_image_t img = {0};
    img.elf_start = elf_start;
    img.entry = eh->e_entry;
    const elf64_phdr_t *phdrs = (const elf64_phdr_t *)(elf_start + eh->e_phoff);
    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        const elf64_phdr_t *ph = &phdrs[i];
        if (ph->p_type != PT_LOAD) {
            continue;
        }
        uint64_t seg_file_end = ph->p_offset + ph->p_filesz;
        if (ph->p_filesz > ph->p_memsz || ph->p_offset > blob_len ||
            seg_file_end > blob_len || seg_file_end < ph->p_offset) {
            kprintf("[elf] PT_LOAD segment out of bounds\n");
            spin_unlock(&elf_lock);
            return NULL;
        }
        if (img.nsegs >= ELF_MAX_LOAD_SEGMENTS) {
            kprintf("[elf] too many PT_LOAD segments (max %d)\n", ELF_MAX_LOAD_SEGMENTS);
            spin_unlock(&elf_lock);
            return NULL;
        }
        elf_segment_t *seg = &img.segs[img.nsegs++];
        seg->vaddr = ph->p_vaddr;
        seg->file_off = ph->p_offset;
        seg->filesz = ph->p_filesz;
        seg->memsz = ph->p_memsz;
        seg->prot = prot_from_pflags(ph->p_flags);
    }
    if (img.nsegs == 0) {
        kprintf("[elf] no PT_LOAD segments\n");
        spin_unlock(&elf_lock);
        return NULL;
    }
    uint32_t slot = elf_cache_next_victim;
    elf_cache_next_victim = (elf_cache_next_victim + 1) % ELF_CACHE_SIZE;
    elf_cache[slot] = img;
    elf_cache[slot].in_use = 1;
    elf_cache_stats.parses++;
    spin_unlock(&elf_lock);
    return &elf_cache[slot];
}
tcb_t *elf_load_and_spawn(const char *name, const uint8_t *elf_start,
                          const uint8_t *elf_end, uint8_t prio, tid_t pager_tid) {
    return elf_load_and_spawn_argv(name, elf_start, elf_end, prio, pager_tid, 0, NULL);
}
static vaddr_t map_segments_and_stack(paddr_t as, const elf_image_t *img,
                                      const uint8_t *elf_start, const char *name) {
    vaddr_t highest_end = 0;
    for (uint32_t i = 0; i < img->nsegs; i++) {
        const elf_segment_t *seg = &img->segs[i];
        vaddr_t seg_lo = seg->vaddr & ~(PAGE_SIZE_4K - 1);
        vaddr_t seg_hi = (seg->vaddr + seg->memsz + PAGE_SIZE_4K - 1) & ~(PAGE_SIZE_4K - 1);
        for (vaddr_t page_va = seg_lo; page_va < seg_hi; page_va += PAGE_SIZE_4K) {
            paddr_t frame = pmm_alloc(PMM_COLOR_ANY);
            if (!frame) {
                kprintf("[elf] out of physical memory loading '%s'\n", name);
                return 0;
            }
            memset((void *)frame, 0, PAGE_SIZE_4K);
            arch_vm_map_page(as, page_va, frame, seg->prot);
            vaddr_t seg_data_lo = seg->vaddr;
            vaddr_t seg_data_hi = seg->vaddr + seg->filesz;
            vaddr_t copy_lo = page_va > seg_data_lo ? page_va : seg_data_lo;
            vaddr_t page_end = page_va + PAGE_SIZE_4K;
            vaddr_t copy_hi = page_end < seg_data_hi ? page_end : seg_data_hi;
            if (copy_hi > copy_lo) {
                uint64_t file_off = seg->file_off + (copy_lo - seg->vaddr);
                void *dst = (void *)(frame + (copy_lo - page_va));
                memcpy(dst, elf_start + file_off, copy_hi - copy_lo);
            }
        }
        if (seg_hi > highest_end) {
            highest_end = seg_hi;
        }
    }
    vaddr_t stack_base = highest_end + PAGE_SIZE_4K;
    vaddr_t stack_top = stack_base + (APP_STACK_PAGES * PAGE_SIZE_4K);
    for (uint32_t i = 0; i < APP_STACK_PAGES; i++) {
        paddr_t stack_frame = pmm_alloc(PMM_COLOR_ANY);
        if (!stack_frame) {
            kprintf("[elf] out of physical memory allocating '%s's stack\n", name);
            return 0;
        }
        memset((void *)stack_frame, 0, PAGE_SIZE_4K);
        arch_vm_map_page(as, stack_base + i * PAGE_SIZE_4K, stack_frame,
                         VM_PROT_READ | VM_PROT_WRITE | VM_PROT_USER);
    }
    return stack_top;
}
static int build_string_array_page(paddr_t as, vaddr_t page_va, int count,
                                   const char *const items[], const char *name,
                                   const char *what, vaddr_t *out_uva) {
    *out_uva = 0;
    if (count <= 0) {
        return 0;
    }
    paddr_t frame = pmm_alloc(PMM_COLOR_ANY);
    if (!frame) {
        kprintf("[elf] out of physical memory allocating '%s's %s\n", name, what);
        return -1;
    }
    arch_vm_map_page(as, page_va, frame, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_USER);
    uint64_t *ptr_array = (uint64_t *)frame;
    uint64_t str_off = (uint64_t)(count + 1) * sizeof(uint64_t);
    for (int i = 0; i < count; i++) {
        uint64_t len = (uint64_t)strlen(items[i]) + 1;
        if (str_off + len > PAGE_SIZE_4K) {
            kprintf("[elf] %s too large for '%s'\n", what, name);
            return -1;
        }
        memcpy((void *)(frame + str_off), items[i], len);
        ptr_array[i] = page_va + str_off;
        str_off += len;
    }
    ptr_array[count] = 0;
    *out_uva = page_va;
    return 0;
}
tcb_t *elf_load_and_spawn_argv(const char *name, const uint8_t *elf_start,
                               const uint8_t *elf_end, uint8_t prio, tid_t pager_tid,
                               int argc, const char *const *argv) {
    const elf_image_t *img = elf_parse(elf_start, elf_end);
    if (!img) {
        return NULL;
    }
    paddr_t as = vm_address_space_create();
    if (!as) {
        kprintf("[elf] out of memory creating an address space for '%s'\n", name);
        return NULL;
    }
    vaddr_t stack_top = map_segments_and_stack(as, img, elf_start, name);
    if (!stack_top) {
        return NULL;
    }
    vaddr_t argv_base = stack_top + PAGE_SIZE_4K;
    vaddr_t heap_base = argv_base + 2 * PAGE_SIZE_4K;
    vaddr_t argv_uva;
    if (build_string_array_page(as, argv_base, argc, argv, name, "argv", &argv_uva) != 0) {
        return NULL;
    }
    return thread_create_user_argv(name, img->entry, stack_top, prio, as, pager_tid,
                                   (uint64_t)argc, argv_uva, 0 ,
                                   heap_base, 0 );
}
#define SPAWN_INFO_RESERVED_TAIL 128
tcb_t *elf_load_and_spawn_req(const char *name, const uint8_t *elf_start,
                              const uint8_t *elf_end, uint8_t prio, tid_t pager_tid,
                              int argc, const char *const *argv,
                              int envc, const char *const *envp,
                              uint32_t nfds, const void *fds_blob) {
    const elf_image_t *img = elf_parse(elf_start, elf_end);
    if (!img) {
        return NULL;
    }
    paddr_t as = vm_address_space_create();
    if (!as) {
        kprintf("[elf] out of memory creating an address space for '%s'\n", name);
        return NULL;
    }
    vaddr_t stack_top = map_segments_and_stack(as, img, elf_start, name);
    if (!stack_top) {
        return NULL;
    }
    vaddr_t argv_base = stack_top + PAGE_SIZE_4K;
    vaddr_t envp_base = argv_base + PAGE_SIZE_4K;
    vaddr_t info_top = envp_base + PAGE_SIZE_4K;
    vaddr_t heap_base = info_top + PAGE_SIZE_4K;
    vaddr_t argv_uva;
    if (build_string_array_page(as, argv_base, argc, argv, name, "argv", &argv_uva) != 0) {
        return NULL;
    }
    paddr_t envp_frame = pmm_alloc(PMM_COLOR_ANY);
    if (!envp_frame) {
        kprintf("[elf] out of physical memory allocating '%s's envp/info page\n", name);
        return NULL;
    }
    arch_vm_map_page(as, envp_base, envp_frame, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_USER);
    vaddr_t envp_uva = 0;
    if (envc > 0) {
        uint64_t *ptr_array = (uint64_t *)envp_frame;
        uint64_t str_off = (uint64_t)(envc + 1) * sizeof(uint64_t);
        for (int i = 0; i < envc; i++) {
            uint64_t len = (uint64_t)strlen(envp[i]) + 1;
            if (str_off + len > PAGE_SIZE_4K - SPAWN_INFO_RESERVED_TAIL) {
                kprintf("[elf] envp too large for '%s'\n", name);
                return NULL;
            }
            memcpy((void *)(envp_frame + str_off), envp[i], len);
            ptr_array[i] = envp_base + str_off;
            str_off += len;
        }
        ptr_array[envc] = 0;
        envp_uva = envp_base;
    }
    vaddr_t spawn_info_va = envp_base + (PAGE_SIZE_4K - SPAWN_INFO_RESERVED_TAIL);
    robu_spawn_info_t *info =
        (robu_spawn_info_t *)(envp_frame + (PAGE_SIZE_4K - SPAWN_INFO_RESERVED_TAIL));
    info->magic = SPAWN_INFO_MAGIC;
    info->nfds = nfds;
    if (nfds > 0) {
        memcpy(info + 1, fds_blob, (size_t)nfds * sizeof(robu_spawn_fd_t));
    }
    tcb_t *child = thread_create_user_argv_locked(name, img->entry, stack_top, prio, as, pager_tid,
                                                  (uint64_t)argc, argv_uva, envp_uva, heap_base,
                                                  spawn_info_va);
    if (child && nfds > 0) {
        const robu_spawn_fd_t *fds = (const robu_spawn_fd_t *)fds_blob;
        for (uint32_t i = 0; i < nfds; i++) {
            if (fds[i].kind == SPAWN_FD_KIND_PIPE_READ) {
                pipe_add_holder(child->tid, fds[i].handle, 0);
            } else if (fds[i].kind == SPAWN_FD_KIND_PIPE_WRITE) {
                pipe_add_holder(child->tid, fds[i].handle, 1);
            }
        }
    }
    return child;
}

int elf_exec_current(tcb_t *cur, const char *name, const uint8_t *elf_start,
                     const uint8_t *elf_end, int argc, const char *const *argv,
                     int envc, const char *const *envp) {
    const elf_image_t *img = elf_parse(elf_start, elf_end);
    if (!img) {
        return -1;
    }
    paddr_t new_as = vm_address_space_create();
    if (!new_as) {
        kprintf("[elf] out of memory creating an address space to exec '%s'\n", name);
        return -1;
    }
    vaddr_t stack_top = map_segments_and_stack(new_as, img, elf_start, name);
    if (!stack_top) {
        vm_address_space_destroy(new_as);
        return -1;
    }
    vaddr_t argv_base = stack_top + PAGE_SIZE_4K;
    vaddr_t envp_base = argv_base + PAGE_SIZE_4K;
    vaddr_t heap_base = envp_base + PAGE_SIZE_4K;
    vaddr_t argv_uva, envp_uva;
    if (build_string_array_page(new_as, argv_base, argc, argv, name, "argv", &argv_uva) != 0) {
        vm_address_space_destroy(new_as);
        return -1;
    }
    if (build_string_array_page(new_as, envp_base, envc, envp, name, "envp", &envp_uva) != 0) {
        vm_address_space_destroy(new_as);
        return -1;
    }
    paddr_t old_as = cur->address_space;
    cur->address_space = new_as;

    arch_uctx_init_user_argv(&cur->uctx, img->entry, stack_top, (uint64_t)argc, argv_uva,
                             envp_uva, heap_base, 0);

    arch_vm_activate(new_as);
    vm_address_space_destroy(old_as);
    return 0;
}
