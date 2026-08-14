#include "smp.h"

.section .rodata
.global ap_trampoline_start, ap_trampoline_end
.align 16
.code16
ap_trampoline_start:
    cli
    cld
    mov %cs, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %ss

    mov $(ap16_gdt_ptr - ap_trampoline_start), %bx
    .byte 0x66
    lgdt (%bx)

    mov %cr0, %eax
    or $1, %eax
    mov %eax, %cr0

    ljmpl $0x08, $(AP_TRAMPOLINE_PADDR + (ap32_start - ap_trampoline_start))

.align 8
ap16_gdt:
    .quad 0
    .quad 0x00CF9A000000FFFF
    .quad 0x00CF92000000FFFF
ap16_gdt_end:
ap16_gdt_ptr:
    .word ap16_gdt_end - ap16_gdt - 1
    .long AP_TRAMPOLINE_PADDR + (ap16_gdt - ap_trampoline_start)

.code32
ap32_start:
    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %ss

    mov %cr4, %eax
    or $((1 << 5) | (1 << 9) | (1 << 10)), %eax
    mov %eax, %cr4

    .extern boot_pml4
    mov $boot_pml4, %eax
    mov %eax, %cr3

    mov $0xC0000080, %ecx
    rdmsr
    or $((1 << 8) | (1 << 11)), %eax
    wrmsr

    mov %cr0, %eax
    and $~(1 << 2), %eax
    or $(0x80000000 | (1 << 1)), %eax
    mov %eax, %cr0

    .extern kernel_gdt_ptr
    lgdt kernel_gdt_ptr
    ljmp $0x08, $ap64_start

.code64
ap64_start:
    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %ss

    .extern ap_next_kstack_top
    movabsq $ap_next_kstack_top, %rax
    movq (%rax), %rsp
    xorq %rbp, %rbp

    .extern ap_entry
    call ap_entry
1:  cli
    hlt
    jmp 1b
ap_trampoline_end:

.section .note.GNU-stack, "", @progbits
