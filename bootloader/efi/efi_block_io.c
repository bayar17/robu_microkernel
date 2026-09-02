#include "efi_types.h"
#include "efi_disk.h"

static EFI_BLOCK_IO_PROTOCOL *g_disk;
static UINT8 *g_bounce;

#define EFI_BOUNCE_SECTORS 32
#define EFI_BOUNCE_PAGES 4

int efi_block_io_init(EFI_BLOCK_IO_PROTOCOL *disk, EFI_BOOT_SERVICES *BootServices) {
    if (!disk || !disk->Media || disk->Media->BlockSize != 512 || !BootServices) {
        return -1;
    }
    EFI_PHYSICAL_ADDRESS addr = 0xFFFFFFFFULL;
    if (EFI_ERROR(BootServices->AllocatePages(AllocateMaxAddress, EfiLoaderData,
                                              EFI_BOUNCE_PAGES, &addr))) {
        return -1;
    }
    g_disk = disk;
    g_bounce = (UINT8 *)(UINTN)addr;
    return 0;
}

int efi_block_io_ready(void) {
    return g_disk != NULL && g_disk->Media != NULL && g_disk->Media->BlockSize == 512 && g_bounce != NULL;
}

unsigned int bios_read_sectors(unsigned int lba, unsigned int count, __UINTPTR_TYPE__ dst) {
    if (!efi_block_io_ready() || count > EFI_BOUNCE_SECTORS) {
        return 0;
    }
    EFI_STATUS status = g_disk->ReadBlocks(g_disk, g_disk->Media->MediaId, (EFI_LBA)lba,
                                            (UINTN)count * 512, g_bounce);
    if (EFI_ERROR(status)) {
        return 0;
    }
    UINT8 *out = (UINT8 *)dst;
    for (UINTN i = 0; i < (UINTN)count * 512; i++) {
        out[i] = g_bounce[i];
    }
    return count;
}
