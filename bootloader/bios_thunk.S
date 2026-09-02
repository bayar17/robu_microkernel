.code32
.section .text
.global bios_read_sectors
.global bios_get_e820

.equ BOOT_DRIVE_ADDR, 0x0500
.equ DAP_ADDR, 0x0510
.equ THUNK_LBA_ADDR, 0x0520
.equ THUNK_COUNT_ADDR, 0x0524
.equ THUNK_DST_ADDR, 0x0528
.equ THUNK_STATUS_ADDR, 0x052c
.equ E820_COUNT_ADDR, 0x0530
.equ E820_DST_ADDR, 0x0534
.equ E820_BUF_PHYS, 0x5000
.equ E820_MAX_ENTRIES, 32
.equ E820_ENTRY_SIZE, 20
.equ STAGING_SEG, 0x0100
.equ STAGING_OFF, 0x0000
.equ STAGING_PHYS, 0x1000
.equ STAGING_MAX_SECTORS, 32
.equ RM_STACK_SEG, 0x0000
.equ RM_STACK_SP, 0x7c00

bios_read_sectors:
    push %ebp
    mov %esp, %ebp
    push %ebx
    push %esi
    push %edi

    mov 8(%ebp), %eax
    mov %eax, THUNK_LBA_ADDR
    mov 12(%ebp), %eax
    cmp $STAGING_MAX_SECTORS, %eax
    jbe bios_read_sectors_count_ok
    mov $STAGING_MAX_SECTORS, %eax
bios_read_sectors_count_ok:
    mov %eax, THUNK_COUNT_ADDR
    mov 16(%ebp), %eax
    mov %eax, THUNK_DST_ADDR

    cli
    ljmp $0x18, $thunk_pm16_entry

.code16
thunk_pm16_entry:
    mov $0x20, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %ss

    mov %cr0, %eax
    and $0xfffffffe, %eax
    mov %eax, %cr0
    ljmp $0x0000, $thunk_rm_entry

thunk_rm_entry:
    mov $RM_STACK_SEG, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %ss
    mov $RM_STACK_SP, %sp
    sti

    movb $0x10, DAP_ADDR
    movb $0x00, DAP_ADDR + 1
    movw THUNK_COUNT_ADDR, %ax
    movw %ax, DAP_ADDR + 2
    movw $STAGING_OFF, DAP_ADDR + 4
    movw $STAGING_SEG, DAP_ADDR + 6
    movl THUNK_LBA_ADDR, %eax
    movl %eax, DAP_ADDR + 8
    movl $0, DAP_ADDR + 12

    movb BOOT_DRIVE_ADDR, %dl
    mov $DAP_ADDR, %si
    movb $0x42, %ah
    int $0x13
    jc thunk_error
    movb $0, THUNK_STATUS_ADDR
    jmp thunk_back_to_pm

thunk_error:
    movb $1, THUNK_STATUS_ADDR

thunk_back_to_pm:
    cli
    mov %cr0, %eax
    or $1, %eax
    mov %eax, %cr0
    ljmp $0x08, $thunk_pm32_return

.code32
thunk_pm32_return:
    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %ss
    mov %ax, %fs
    mov %ax, %gs

    cmpb $0, THUNK_STATUS_ADDR
    jne bios_read_sectors_fail

    mov $STAGING_PHYS, %esi
    mov THUNK_DST_ADDR, %edi
    mov THUNK_COUNT_ADDR, %ecx
    shl $9, %ecx
    rep movsb

    mov THUNK_COUNT_ADDR, %eax
    jmp bios_read_sectors_done

bios_read_sectors_fail:
    xor %eax, %eax

bios_read_sectors_done:
    mov -12(%ebp), %edi
    mov -8(%ebp), %esi
    mov -4(%ebp), %ebx
    mov %ebp, %esp
    pop %ebp
    ret

bios_get_e820:
    push %ebp
    mov %esp, %ebp
    push %ebx
    push %esi
    push %edi

    mov 8(%ebp), %eax
    mov %eax, E820_DST_ADDR

    cli
    ljmp $0x18, $e820_pm16_entry

.code16
e820_pm16_entry:
    mov $0x20, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %ss

    mov %cr0, %eax
    and $0xfffffffe, %eax
    mov %eax, %cr0
    ljmp $0x0000, $e820_rm_entry

e820_rm_entry:
    xor %ax, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %ss
    mov $RM_STACK_SP, %sp
    sti

    xor %ebx, %ebx
    movl $0, E820_COUNT_ADDR

e820_loop:
    movl E820_COUNT_ADDR, %ecx
    cmpl $E820_MAX_ENTRIES, %ecx
    jae e820_done

    imul $E820_ENTRY_SIZE, %ecx, %edi
    add $E820_BUF_PHYS, %edi

    mov $0x534d4150, %edx
    mov $E820_ENTRY_SIZE, %ecx
    mov $0xe820, %eax
    int $0x15
    jc e820_done
    cmp $0x534d4150, %eax
    jne e820_done

    movl E820_COUNT_ADDR, %eax
    inc %eax
    movl %eax, E820_COUNT_ADDR

    cmp $0, %ebx
    je e820_done
    jmp e820_loop

e820_done:
    cli
    mov %cr0, %eax
    or $1, %eax
    mov %eax, %cr0
    ljmp $0x08, $e820_pm32_return

.code32
e820_pm32_return:
    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %ss
    mov %ax, %fs
    mov %ax, %gs

    mov $E820_BUF_PHYS, %esi
    mov E820_DST_ADDR, %edi
    mov E820_COUNT_ADDR, %ecx
    imul $E820_ENTRY_SIZE, %ecx, %ecx
    rep movsb

    mov E820_COUNT_ADDR, %eax

    mov -12(%ebp), %edi
    mov -8(%ebp), %esi
    mov -4(%ebp), %ebx
    mov %ebp, %esp
    pop %ebp
    ret
