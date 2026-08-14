
#define MULTIBOOT2_HEADER_MAGIC 0xe85250d6
#define MULTIBOOT2_ARCH_I386    0

.section .multiboot2, "a"
.align 8
mb2_header_start:
    .long MULTIBOOT2_HEADER_MAGIC
    .long MULTIBOOT2_ARCH_I386
    .long mb2_header_end - mb2_header_start
    .long -(MULTIBOOT2_HEADER_MAGIC + MULTIBOOT2_ARCH_I386 + (mb2_header_end - mb2_header_start))

    .align 8
    .short 5
    .short 1
    .long 20
    .long 1024
    .long 768
    .long 32

    .align 8
    .short 0
    .short 0
    .long 8
mb2_header_end:

.section .text
.code32
.global mb2_start
.type mb2_start, @function
mb2_start:
    cli

    movl %eax, %eax
    movl %ebx, %ebx

    .extern start
    jmp start

.section .note.GNU-stack, "", @progbits
