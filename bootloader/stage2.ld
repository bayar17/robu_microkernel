ENTRY(_start2)
SECTIONS
{
    . = 0x8000;
    .text : {
        *(.text) *(.text.*)
        *(.rodata) *(.rodata.*)
        *(.data) *(.data.*)
    }
    .bss : { *(.bss) *(.bss.*) *(COMMON) }
    /DISCARD/ : { *(.note.GNU-stack) *(.comment) *(.eh_frame) }
}
