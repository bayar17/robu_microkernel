#include "efi_types.h"
#include "efi_disk.h"
#include "efi_print.h"
#include "efi_kernel_load.h"
#include "ext2_read.h"

extern unsigned int bios_read_sectors(unsigned int lba, unsigned int count, __UINTPTR_TYPE__ dst);

EFI_STATUS EFIAPI efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    efi_print(SystemTable, (CHAR16 *)L"Robu EFI stub alive\r\n");

    EFI_BLOCK_IO_PROTOCOL *disk = efi_find_whole_disk_block_io(ImageHandle, SystemTable);
    if (!disk) {
        efi_print(SystemTable, (CHAR16 *)L"FATAL: could not find whole-disk Block IO handle\r\n");
        for (;;) {
        }
    }
    if (disk->Media->BlockSize != 512) {
        efi_print(SystemTable, (CHAR16 *)L"FATAL: disk BlockSize is not 512\r\n");
        for (;;) {
        }
    }
    if (efi_block_io_init(disk, SystemTable->BootServices) != 0) {
        efi_print(SystemTable, (CHAR16 *)L"FATAL: could not allocate EFI Block IO bounce buffer\r\n");
        for (;;) {
        }
    }
    efi_print(SystemTable, (CHAR16 *)L"Block IO: whole-disk handle found, BlockSize=512\r\n");

    if (ext2_mount() != 0) {
        efi_print(SystemTable, (CHAR16 *)L"FATAL: ext2_mount failed\r\n");
        static UINT8 sb_dbg[1024];
        unsigned int got = bios_read_sectors(2050, 2, (__UINTPTR_TYPE__)sb_dbg);
        efi_print(SystemTable, (CHAR16 *)L"  superblock read: got=");
        efi_print_hex32(SystemTable, got);
        efi_print(SystemTable, (CHAR16 *)L" magic=");
        efi_print_hex32(SystemTable, (unsigned int)sb_dbg[56] | ((unsigned int)sb_dbg[57] << 8));
        efi_print(SystemTable, (CHAR16 *)L"\r\n");
        for (;;) {
        }
    }
    efi_print(SystemTable, (CHAR16 *)L"ext2: mounted ok\r\n");

    efi_boot_kernel(ImageHandle, SystemTable);

    for (;;) {
    }
    return EFI_SUCCESS;
}
