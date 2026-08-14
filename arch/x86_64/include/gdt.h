#ifndef ARCH_X86_64_GDT_H
#define ARCH_X86_64_GDT_H
#define GDT_SEL_KCODE  0x08
#define GDT_SEL_KDATA  0x10
#define GDT_SEL_UCODE  0x18
#define GDT_SEL_UDATA  0x20
#define GDT_SEL_TSS    0x28
#define GDT_SEL_TSS_CPU(cpu_id) ((uint16_t)((5 + 2 * (cpu_id)) * 8))
#define GDT_SEL_UCODE_RPL3 (GDT_SEL_UCODE | 3)
#define GDT_SEL_UDATA_RPL3 (GDT_SEL_UDATA | 3)
#endif
