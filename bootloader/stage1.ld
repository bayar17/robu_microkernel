ENTRY(_start)
SECTIONS
{
    . = 0x7c00;
    .text : {
        *(.text) *(.text.*)
        *(.rodata) *(.rodata.*)
        *(.data) *(.data.*)
    }
    .bss : { *(.bss) *(.bss.*) *(COMMON) }
    /DISCARD/ : { *(.note.GNU-stack) *(.comment) *(.eh_frame) }
}
