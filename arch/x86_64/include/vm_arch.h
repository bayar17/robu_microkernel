#ifndef ARCH_X86_64_VM_H
#define ARCH_X86_64_VM_H
#include "robu/vm.h"
#define X86_PTE_P       (1ULL << 0)
#define X86_PTE_RW      (1ULL << 1)
#define X86_PTE_US      (1ULL << 2)
#define X86_PTE_PWT     (1ULL << 3)
#define X86_PTE_PCD     (1ULL << 4)
#define X86_PTE_A       (1ULL << 5)
#define X86_PTE_D       (1ULL << 6)
#define X86_PTE_PS      (1ULL << 7)
#define X86_PTE_PAT     (1ULL << 7)
#define X86_PTE_GLOBAL  (1ULL << 8)
#define X86_PTE_NX      (1ULL << 63)
#define PAGE_SHIFT_4K   12
#define PAGE_MASK_4K    (~(PAGE_SIZE_4K - 1))
#define PAGE_SHIFT_2M   21
#define PAGE_MASK_2M    (~(PAGE_SIZE_2M - 1))
#define PML4_INDEX(vaddr) (((vaddr) >> 39) & 0x1FF)
#define PDPT_INDEX(vaddr) (((vaddr) >> 30) & 0x1FF)
#define PD_INDEX(vaddr)   (((vaddr) >> 21) & 0x1FF)
#define PT_INDEX(vaddr)   (((vaddr) >> 12) & 0x1FF)
typedef uint64_t pte_t;
static inline void arch_invlpg(vaddr_t vaddr) {
    asm volatile("invlpg (%0)" ::"r" (vaddr) : "memory");
}
static inline void arch_load_cr3(paddr_t pml4_phys) {
    asm volatile("mov %0, %%cr3" :: "r" (pml4_phys) : "memory");
}
static inline vaddr_t arch_read_cr2(void) {
    vaddr_t cr2;
    asm volatile("mov %%cr2, %0" : "=r" (cr2));
    return cr2;
}
#endif
