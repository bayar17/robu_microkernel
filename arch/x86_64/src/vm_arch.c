#include "vm_arch.h"
#include "robu/vm.h"
#include "robu/pmm.h"
#include "robu/arch.h"
#include "robu/spinlock.h"
#include "percpu.h"
#include "lapic.h"
#include "robu/kinfo.h"
#include "robu/untyped.h"
extern paddr_t boot_pml4;
extern paddr_t boot_pdpt;
static spinlock_t vm_lock = SPINLOCK_INIT;
static paddr_t alloc_table(void) {
    return pmm_alloc(PMM_COLOR_ANY);
}
paddr_t arch_vm_create_address_space(void) {
    spin_lock(&vm_lock);
    paddr_t pml4 = alloc_table();
    paddr_t pdpt = alloc_table();
    if (!pml4 || !pdpt) {
        spin_unlock(&vm_lock);
        return 0;
    }
    memset((void *)pml4, 0, PAGE_SIZE_4K);
    memset((void *)pdpt, 0, PAGE_SIZE_4K);
    pte_t *pdpt_table = (pte_t *)pdpt;
    pte_t *kernel_pdpt = (pte_t *)&boot_pdpt;
    pdpt_table[0] = kernel_pdpt[0];
    if (kernel_pdpt[PDPT_INDEX(LAPIC_PADDR)]) {
        pdpt_table[PDPT_INDEX(LAPIC_PADDR)] = kernel_pdpt[PDPT_INDEX(LAPIC_PADDR)];
    }
    if (kernel_pdpt[PDPT_INDEX(KINFO_VA)]) {
        pdpt_table[PDPT_INDEX(KINFO_VA)] = kernel_pdpt[PDPT_INDEX(KINFO_VA)];
    }
    pte_t *pml4_table = (pte_t *)pml4;
    pml4_table[0] = pdpt | X86_PTE_P | X86_PTE_RW | X86_PTE_US;
    spin_unlock(&vm_lock);
    return pml4;
}
void arch_vm_destroy_address_space(paddr_t aspace) {
    paddr_t untyped_base;
    uint64_t untyped_size;
    untyped_range(&untyped_base, &untyped_size);
    spin_lock(&vm_lock);
    pte_t *pml4 = (pte_t *)aspace;
    pte_t *kernel_pdpt = (pte_t *)&boot_pdpt;
    if (pml4[0] & X86_PTE_P) {
        pte_t *pdpt = (pte_t *)(pml4[0] & PAGE_MASK_4K);
        for (int i = 0; i < 512; i++) {
            if (!(pdpt[i] & X86_PTE_P)) {
                continue;
            }
            if ((pdpt[i] & PAGE_MASK_4K) == (kernel_pdpt[i] & PAGE_MASK_4K)) {
                continue;
            }
            pte_t *pd = (pte_t *)(pdpt[i] & PAGE_MASK_4K);
            for (int j = 0; j < 512; j++) {
                if (!(pd[j] & X86_PTE_P)) {
                    continue;
                }
                if (pd[j] & X86_PTE_PS) {
                    paddr_t leaf = pd[j] & PAGE_MASK_2M & ~X86_PTE_NX;
                    if (leaf < untyped_base || leaf >= untyped_base + untyped_size) {
                        pmm_free(leaf);
                    }
                    continue;
                }
                pte_t *pt = (pte_t *)(pd[j] & PAGE_MASK_4K);
                for (int k = 0; k < 512; k++) {
                    if (!(pt[k] & X86_PTE_P)) {
                        continue;
                    }
                    paddr_t leaf = pt[k] & PAGE_MASK_4K & ~X86_PTE_NX;
                    if (leaf < untyped_base || leaf >= untyped_base + untyped_size) {
                        pmm_free(leaf);
                    }
                }
                pmm_free((paddr_t)pt);
            }
            pmm_free((paddr_t)pd);
        }
        pmm_free((paddr_t)pdpt);
    }
    pmm_free(aspace);
    spin_unlock(&vm_lock);
}
paddr_t arch_vm_clone_address_space(paddr_t src) {
    paddr_t dst = arch_vm_create_address_space();
    if (!dst) {
        return 0;
    }
    pte_t *src_pml4 = (pte_t *)src;
    pte_t *kernel_pdpt = (pte_t *)&boot_pdpt;
    if (!(src_pml4[0] & X86_PTE_P)) {
        return dst;
    }
    pte_t *src_pdpt = (pte_t *)(src_pml4[0] & PAGE_MASK_4K);
    for (int i = 0; i < 512; i++) {
        if (!(src_pdpt[i] & X86_PTE_P)) {
            continue;
        }
        if ((src_pdpt[i] & PAGE_MASK_4K) == (kernel_pdpt[i] & PAGE_MASK_4K)) {
            continue;
        }
        pte_t *src_pd = (pte_t *)(src_pdpt[i] & PAGE_MASK_4K);
        for (int j = 0; j < 512; j++) {
            if (!(src_pd[j] & X86_PTE_P)) {
                continue;
            }
            if (src_pd[j] & X86_PTE_PS) {
                continue;
            }
            pte_t *src_pt = (pte_t *)(src_pd[j] & PAGE_MASK_4K);
            for (int k = 0; k < 512; k++) {
                if (!(src_pt[k] & X86_PTE_P)) {
                    continue;
                }
                paddr_t src_frame = src_pt[k] & PAGE_MASK_4K & ~X86_PTE_NX;
                paddr_t dst_frame = pmm_alloc(PMM_COLOR_ANY);
                if (!dst_frame) {
                    return 0;
                }
                memcpy((void *)dst_frame, (const void *)src_frame, PAGE_SIZE_4K);
                vaddr_t vaddr = ((vaddr_t)i << 30) | ((vaddr_t)j << 21) | ((vaddr_t)k << 12);
                uint32_t prot = VM_PROT_READ;
                if (src_pt[k] & X86_PTE_RW) prot |= VM_PROT_WRITE;
                if (src_pt[k] & X86_PTE_US) prot |= VM_PROT_USER;
                if (!(src_pt[k] & X86_PTE_NX)) prot |= VM_PROT_EXEC;
                arch_vm_map_page(dst, vaddr, dst_frame, prot);
            }
        }
    }
    return dst;
}
static pte_t *get_next_level(pte_t *table, uint32_t index) {
    if (!(table[index] & X86_PTE_P)) {
        paddr_t new_table = alloc_table();
        memset((void *)new_table, 0, PAGE_SIZE_4K);
        table[index] = new_table | X86_PTE_P | X86_PTE_RW | X86_PTE_US;
    }
    return (pte_t *)(table[index] & PAGE_MASK_4K);
}
int arch_vm_map_large_page(paddr_t aspace, vaddr_t vaddr, paddr_t paddr, uint32_t flags) {
    spin_lock(&vm_lock);
    pte_t *pml4 = (pte_t *)aspace;
    pte_t *pdpt = get_next_level(pml4, PML4_INDEX(vaddr));
    pte_t *pd = get_next_level(pdpt, PDPT_INDEX(vaddr));
    uint32_t pd_index = PD_INDEX(vaddr);
    uint64_t x86_flags = X86_PTE_P | X86_PTE_PS;
    if (flags & VM_PROT_WRITE) x86_flags |= X86_PTE_RW;
    if (flags & VM_PROT_USER)  x86_flags |= X86_PTE_US;
    if (!(flags & VM_PROT_EXEC)) x86_flags |= X86_PTE_NX;
    pd[pd_index] = (paddr & PAGE_MASK_2M) | x86_flags;
    arch_invlpg(vaddr);
    spin_unlock(&vm_lock);
    return 0;
}
int arch_vm_map_page(paddr_t aspace, vaddr_t vaddr, paddr_t paddr, uint32_t flags) {
    spin_lock(&vm_lock);
    pte_t *pml4 = (pte_t *)aspace;
    pte_t *pdpt = get_next_level(pml4, PML4_INDEX(vaddr));
    pte_t *pd = get_next_level(pdpt, PDPT_INDEX(vaddr));
    pte_t *pt = get_next_level(pd, PD_INDEX(vaddr));
    uint32_t pt_index = PT_INDEX(vaddr);
    uint64_t x86_flags = X86_PTE_P;
    if (flags & VM_PROT_WRITE) x86_flags |= X86_PTE_RW;
    if (flags & VM_PROT_USER)  x86_flags |= X86_PTE_US;
    if (!(flags & VM_PROT_EXEC)) x86_flags |= X86_PTE_NX;
    pt[pt_index] = (paddr & PAGE_MASK_4K) | x86_flags;
    arch_invlpg(vaddr);
    spin_unlock(&vm_lock);
    return 0;
}
static pte_t *walk_to_pte(pte_t *pml4, vaddr_t vaddr) {
    pte_t *pdpt_e = &pml4[PML4_INDEX(vaddr)];
    if (!(*pdpt_e & X86_PTE_P)) return NULL;
    pte_t *pdpt = (pte_t *)(*pdpt_e & PAGE_MASK_4K);
    pte_t *pd_e = &pdpt[PDPT_INDEX(vaddr)];
    if (!(*pd_e & X86_PTE_P)) return NULL;
    pte_t *pd = (pte_t *)(*pd_e & PAGE_MASK_4K);
    pte_t *pt_e = &pd[PD_INDEX(vaddr)];
    if (!(*pt_e & X86_PTE_P)) return NULL;
    if (*pt_e & X86_PTE_PS) return pt_e;
    pte_t *pt = (pte_t *)(*pt_e & PAGE_MASK_4K);
    return &pt[PT_INDEX(vaddr)];
}
paddr_t arch_vm_unmap_page(paddr_t aspace, vaddr_t vaddr) {
    spin_lock(&vm_lock);
    pte_t *pml4 = (pte_t *)aspace;
    pte_t *pte = walk_to_pte(pml4, vaddr);
    if (!pte || !(*pte & X86_PTE_P)) {
        spin_unlock(&vm_lock);
        return 0;
    }
    paddr_t frame = *pte & (*pte & X86_PTE_PS ? PAGE_MASK_2M : PAGE_MASK_4K);
    *pte = 0;
    arch_invlpg(vaddr);
    spin_unlock(&vm_lock);
    return frame;
}
paddr_t arch_vm_translate(paddr_t aspace, vaddr_t vaddr) {
    spin_lock(&vm_lock);
    pte_t *pml4 = (pte_t *)aspace;
    pte_t *pte = walk_to_pte(pml4, vaddr);
    if (!pte || !(*pte & X86_PTE_P)) {
        spin_unlock(&vm_lock);
        return 0;
    }
    paddr_t frame = *pte & (*pte & X86_PTE_PS ? PAGE_MASK_2M : PAGE_MASK_4K) & ~X86_PTE_NX;
    spin_unlock(&vm_lock);
    return frame;
}
vm_activate_stats_t vm_activate_stats;
void arch_vm_activate(paddr_t aspace) {
    paddr_t target = aspace ? aspace : (paddr_t)&boot_pml4;
    percpu_t *cpu = this_cpu();
    if (target == cpu->loaded_cr3) {
        vm_activate_stats.skips++;
        return;
    }
    arch_load_cr3(target);
    cpu->loaded_cr3 = target;
    vm_activate_stats.loads++;
}
