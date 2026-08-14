#ifndef ARCH_X86_64_UACPI_GLUE_H
#define ARCH_X86_64_UACPI_GLUE_H
#include "robu/types.h"
int uacpi_glue_shutdown(void);
int uacpi_glue_reboot(void);
int acpi_enumerate_cpus(uint32_t *apic_ids, int max_ids);
#endif
