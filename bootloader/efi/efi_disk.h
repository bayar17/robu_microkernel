#ifndef ROBU_EFI_DISK_H
#define ROBU_EFI_DISK_H

#include "efi_types.h"

EFI_BLOCK_IO_PROTOCOL *efi_find_whole_disk_block_io(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable);
int efi_block_io_init(EFI_BLOCK_IO_PROTOCOL *disk, EFI_BOOT_SERVICES *BootServices);
int efi_block_io_ready(void);

#endif
