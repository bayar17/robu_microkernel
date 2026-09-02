.code16
.section .text
.global _start2
_start2:
    cli
    lgdt gdt_ptr
    mov %cr0, %eax
    or $1, %eax
    mov %eax, %cr0
    ljmp $0x08, $pm_entry

.code32
pm_entry:
    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %ss
    mov %ax, %fs
    mov %ax, %gs
    mov $0x90000, %esp

    call stage2_main
hang2:
    cli
    hlt
    jmp hang2

.align 8
gdt_start:
    .quad 0
    .word 0xffff
    .word 0x0000
    .byte 0x00
    .byte 0x9a
    .byte 0xcf
    .byte 0x00
    .word 0xffff
    .word 0x0000
    .byte 0x00
    .byte 0x92
    .byte 0xcf
    .byte 0x00
    .word 0xffff
    .word 0x0000
    .byte 0x00
    .byte 0x9a
    .byte 0x8f
    .byte 0x00
    .word 0xffff
    .word 0x0000
    .byte 0x00
    .byte 0x92
    .byte 0x8f
    .byte 0x00
gdt_end:

gdt_ptr:
    .word gdt_end - gdt_start - 1
    .long gdt_start
