#ifndef ROBU_EFI_KERNEL_LOAD_H
#define ROBU_EFI_KERNEL_LOAD_H

#include "efi_types.h"

void efi_boot_kernel(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable);

#endif
