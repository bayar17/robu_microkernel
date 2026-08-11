ENTRY(_start)

PHDRS
{
    text PT_LOAD FILEHDR PHDRS;
    rodata PT_LOAD;
    data PT_LOAD;
    tls PT_TLS;
}

SECTIONS
{
    . = 0x0000000060000000;

    .text : {
        *(.text .text.*)
        *(.iplt)
    } :text

    . = ALIGN(4096);
    .rodata : {
        *(.rodata .rodata.*)
        *(.gnu.warning.*)
    } :rodata

    .rela.dyn : {
        *(.rela.dyn)
    } :rodata

    . = ALIGN(4096);
    .data.rel.ro : {
        PROVIDE_HIDDEN(__init_array_start = .);
        *(.init_array*)
        PROVIDE_HIDDEN(__init_array_end = .);
        PROVIDE_HIDDEN(__fini_array_start = .);
        *(.fini_array*)
        PROVIDE_HIDDEN(__fini_array_end = .);
        *(.got)
        *(.got.plt)
    } :data

    .tdata : {
        *(.tdata .tdata.*)
    } :data :tls
    .tbss : {
        *(.tbss .tbss.*)
    } :data :tls
    . = . + SIZEOF(.tbss);

    .data : {
        *(.data .data.*)
    } :data

    . = ALIGN(4096);
    .bss : {
        *(COMMON)
        *(.bss .bss.*)
    } :data
}
