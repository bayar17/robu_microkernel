#include "efi_types.h"
#include "efi_memmap.h"

#define RAW_MAP_BYTES 16384
#define MAX_E820_ENTRIES 128

static UINT8 g_raw_map[RAW_MAP_BYTES];
static UINT8 g_e820_buf[MAX_E820_ENTRIES * 20];

static int is_available_type(UINT32 type) {
    return type == EfiConventionalMemory || type == EfiBootServicesCode ||
           type == EfiBootServicesData || type == EfiLoaderCode || type == EfiLoaderData;
}

unsigned int efi_build_e820_map(EFI_SYSTEM_TABLE *SystemTable, unsigned long long *out_buf,
                                 UINTN *out_map_key) {
    EFI_BOOT_SERVICES *BS = SystemTable->BootServices;
    UINTN map_size = RAW_MAP_BYTES;
    UINTN map_key = 0;
    UINTN desc_size = 0;
    UINT32 desc_ver = 0;
    EFI_STATUS status = BS->GetMemoryMap(&map_size, (EFI_MEMORY_DESCRIPTOR *)g_raw_map, &map_key,
                                          &desc_size, &desc_ver);
    if (EFI_ERROR(status) || desc_size == 0) {
        return 0;
    }
    if (out_map_key) {
        *out_map_key = map_key;
    }

    UINTN count = map_size / desc_size;
    unsigned int out_count = 0;
    for (UINTN i = 0; i < count && out_count < MAX_E820_ENTRIES; i++) {
        EFI_MEMORY_DESCRIPTOR *d = (EFI_MEMORY_DESCRIPTOR *)(g_raw_map + i * desc_size);
        UINT32 e820_type = is_available_type(d->Type) ? 1u : 2u;
        UINT64 base = d->PhysicalStart;
        UINT64 len = d->NumberOfPages * 4096ULL;
        UINT8 *dst = g_e820_buf + out_count * 20;
        for (int j = 0; j < 8; j++) {
            dst[j] = (UINT8)(base >> (8 * j));
        }
        for (int j = 0; j < 8; j++) {
            dst[8 + j] = (UINT8)(len >> (8 * j));
        }
        for (int j = 0; j < 4; j++) {
            dst[16 + j] = (UINT8)(e820_type >> (8 * j));
        }
        out_count++;
    }

    *out_buf = (unsigned long long)(UINTN)g_e820_buf;
    return out_count;
}
