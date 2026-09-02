.text
.global efi_mode_transition_and_jump

.code64
efi_mode_transition_and_jump:
    cli
    mov %ecx, %esi
    mov %edx, %edi
    mov %r8d, %ebp

    lea gdt_ptr(%rip), %rax
    lgdt (%rax)

    push $0x08
    push %rbp
    lretq

.code32
.global efi_pm32_template_start
.global efi_pm32_template_end
efi_pm32_template_start:
    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs
    mov %ax, %gs
    mov %ax, %ss

    mov %cr0, %eax
    btr $31, %eax
    mov %eax, %cr0

    mov %cr4, %eax
    btr $12, %eax
    btr $17, %eax
    mov %eax, %cr4

    mov $0xC0000080, %ecx
    rdmsr
    btr $8, %eax
    wrmsr

    mov %esi, %ecx
    mov %edi, %ebx
    mov $0x36d76289, %eax
    cli
    jmp *%ecx

efi_pm32_template_end:

.align 8
gdt:
    .quad 0x0000000000000000
    .quad 0x00CF9A000000FFFF
    .quad 0x00CF92000000FFFF
gdt_end:

.align 4
gdt_ptr:
    .word gdt_end - gdt - 1
    .quad gdt
