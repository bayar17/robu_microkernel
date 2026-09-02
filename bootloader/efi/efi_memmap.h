#ifndef ROBU_EFI_MEMMAP_H
#define ROBU_EFI_MEMMAP_H

#include "efi_types.h"

unsigned int efi_build_e820_map(EFI_SYSTEM_TABLE *SystemTable, unsigned long long *out_buf,
                                 UINTN *out_map_key);

#endif
