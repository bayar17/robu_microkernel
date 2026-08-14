.section .note.Xen, "a", @note
.align 4
    .long 4
    .long 4
    .long 18
    .asciz "Xen"
    .long start

#define PTE_P   (1 << 0)
#define PTE_W   (1 << 1)
#define PTE_PS  (1 << 7)

.section .bss

.align 16
.global kstack_bottom
.global kstack_top
kstack_bottom:
    .skip 16384
kstack_top:

.align 4096
.global boot_pml4
boot_pml4:
    .skip 4096
.global boot_pdpt
boot_pdpt:
    .skip 4096
boot_pd:
    .skip 4096

.align 8
.global pvh_start_info_ptr
pvh_start_info_ptr:
    .skip 8

.global multiboot2_info_ptr
multiboot2_info_ptr:
    .skip 8

.section .rodata
.align 8
gdt64:
    .quad 0
    .quad 0x00209A0000000000
    .quad 0x0000920000000000
gdt64_end:
gdt64_ptr:
    .word gdt64_end - gdt64 - 1
    .long gdt64

.section .text
.code32
.global start
.type start, @function
start:
    cli

    cmpl $0x36d76289, %eax
    je 1f
    movl %ebx, pvh_start_info_ptr
    jmp 2f
1:
    movl %ebx, multiboot2_info_ptr
2:

    movl $kstack_top, %esp

    movl $boot_pdpt + (PTE_P | PTE_W), boot_pml4
    movl $boot_pd + (PTE_P | PTE_W), boot_pdpt

    movl $boot_pd, %edi
    movl $(PTE_P | PTE_W | PTE_PS), %eax
    movl $512, %ecx
3:
    movl %eax, (%edi)
    addl $0x200000, %eax
    addl $8, %edi
    loop 3b

    movl $boot_pml4, %eax
    movl %eax, %cr3

    movl %cr4, %eax
    orl $((1 << 5) | (1 << 9) | (1 << 10)), %eax
    movl %eax, %cr4
    movl $0xC0000080, %ecx
    rdmsr
    orl $((1 << 8) | (1 << 11)), %eax
    wrmsr
    movl %cr0, %eax
    andl $~(1 << 2), %eax
    orl $(0x80000000 | (1 << 1)), %eax
    movl %eax, %cr0

    lgdt gdt64_ptr
    ljmp $0x08, $long_entry

.code64
long_entry:
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    xorw %ax, %ax
    movw %ax, %fs
    movw %ax, %gs

    movq $kstack_top, %rsp
    xorq %rbp, %rbp

    .extern kmain
    call kmain

    cli
4:  hlt
    jmp 4b

.section .note.GNU-stack, "", @progbits
