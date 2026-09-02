#ifndef ROBU_EFI_PRINT_H
#define ROBU_EFI_PRINT_H

#include "efi_types.h"

void efi_print(EFI_SYSTEM_TABLE *SystemTable, const CHAR16 *s);
void efi_print_hex32(EFI_SYSTEM_TABLE *SystemTable, unsigned int v);
void efi_print_hex64(EFI_SYSTEM_TABLE *SystemTable, unsigned long long v);

#endif
