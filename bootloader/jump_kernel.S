.code32
.section .text
.global jump_to_kernel
jump_to_kernel:
    mov 4(%esp), %ecx
    mov 8(%esp), %ebx
    mov $0x36d76289, %eax
    cli
    jmp *%ecx
