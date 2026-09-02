.code16
.section .text
.global _start

.equ STAGE2_LOAD_SEG, 0x0800
.equ STAGE2_LOAD_OFF, 0x0000
.equ STAGE2_START_LBA, 34
.equ STAGE2_SECTOR_COUNT, 256
.equ CHUNK_SECTORS, 32

_start:
    cli
    xor %ax, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %ss
    mov $0x7c00, %sp
    mov %dl, boot_drive
    mov %dl, 0x500
    sti

    ljmp $0x0000, $normalize
normalize:
    mov $msg_stage1, %si
    call print_string

    call enable_a20
    call load_stage2

    ljmp $STAGE2_LOAD_SEG, $STAGE2_LOAD_OFF

print_string:
    push %ax
    push %bx
print_string_loop:
    lodsb
    test %al, %al
    jz print_string_done
    mov $0x0e, %ah
    mov $0x00, %bh
    int $0x10
    jmp print_string_loop
print_string_done:
    pop %bx
    pop %ax
    ret

enable_a20:
    push %ax
    in $0x92, %al
    test $0x02, %al
    jnz a20_done
    or $0x02, %al
    and $0xfe, %al
    out %al, $0x92
a20_done:
    pop %ax
    ret

disk_error:
    mov $msg_disk_err, %si
    call print_string
disk_error_hang:
    cli
    hlt
    jmp disk_error_hang

load_stage2:
    pusha

    movw $STAGE2_SECTOR_COUNT, remaining
    movl $STAGE2_START_LBA, cur_lba
    movw $STAGE2_LOAD_SEG, cur_seg

load_chunk:
    movw remaining, %cx
    test %cx, %cx
    jz load_done
    cmpw $CHUNK_SECTORS, %cx
    jbe chunk_size_ok
    movw $CHUNK_SECTORS, %cx
chunk_size_ok:
    movw %cx, dap_count

    movl cur_lba, %eax
    movl %eax, dap_lba_lo
    movl $0, dap_lba_hi

    movw $0x0000, dap_offset
    movw cur_seg, %ax
    movw %ax, dap_segment

    movb boot_drive, %dl
    mov $dap, %si
    movb $0x42, %ah
    int $0x13
    jc disk_error

    movw dap_count, %ax
    subw %ax, remaining

    movzwl %ax, %eax
    addl %eax, cur_lba

    movw dap_count, %ax
    shlw $5, %ax
    addw %ax, cur_seg

    jmp load_chunk

load_done:
    popa
    ret

.align 4
boot_drive:
    .byte 0
remaining:
    .word 0
cur_lba:
    .long 0
cur_seg:
    .word 0

.align 4
dap:
    .byte 0x10
    .byte 0x00
dap_count:
    .word 0
dap_offset:
    .word 0
dap_segment:
    .word 0
dap_lba_lo:
    .long 0
dap_lba_hi:
    .long 0

msg_stage1:
    .asciz "Robu stage1: loading stage2...\r\n"
msg_disk_err:
    .asciz "Robu stage1: disk read error\r\n"

.org 510
.word 0xaa55
