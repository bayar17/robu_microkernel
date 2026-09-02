#include "efi_types.h"
#include "efi_disk.h"

static UINT16 dp_node_len(const EFI_DEVICE_PATH_PROTOCOL *node) {
    return (UINT16)node->Length[0] | ((UINT16)node->Length[1] << 8);
}

static int dp_is_end(const EFI_DEVICE_PATH_PROTOCOL *node) {
    return node->Type == EFI_DEVICE_PATH_TYPE_END && node->SubType == EFI_DEVICE_PATH_SUBTYPE_END_ENTIRE;
}

static UINTN dp_prefix_len(const EFI_DEVICE_PATH_PROTOCOL *path) {
    const UINT8 *base = (const UINT8 *)path;
    UINTN offset = 0;
    UINTN last_node_offset = 0;
    for (;;) {
        const EFI_DEVICE_PATH_PROTOCOL *node = (const EFI_DEVICE_PATH_PROTOCOL *)(base + offset);
        if (dp_is_end(node)) {
            return last_node_offset;
        }
        last_node_offset = offset;
        UINT16 len = dp_node_len(node);
        if (len < 4) {
            return last_node_offset;
        }
        offset += len;
    }
}

static UINTN dp_full_len_excl_end(const EFI_DEVICE_PATH_PROTOCOL *path) {
    const UINT8 *base = (const UINT8 *)path;
    UINTN offset = 0;
    for (;;) {
        const EFI_DEVICE_PATH_PROTOCOL *node = (const EFI_DEVICE_PATH_PROTOCOL *)(base + offset);
        if (dp_is_end(node)) {
            return offset;
        }
        UINT16 len = dp_node_len(node);
        if (len < 4) {
            return offset;
        }
        offset += len;
    }
}

static int bytes_equal(const void *a, const void *b, UINTN len) {
    const UINT8 *pa = (const UINT8 *)a;
    const UINT8 *pb = (const UINT8 *)b;
    for (UINTN i = 0; i < len; i++) {
        if (pa[i] != pb[i]) {
            return 0;
        }
    }
    return 1;
}

static const char g_robu_partition_name[] = "robu-root";

static int is_robu_disk(EFI_BLOCK_IO_PROTOCOL *block_io) {
    if (!block_io->Media || block_io->Media->BlockSize == 0) {
        return 0;
    }
    static UINT8 buf[512];
    UINTN read_size = block_io->Media->BlockSize;
    if (read_size > sizeof(buf)) {
        return 0;
    }
    if (EFI_ERROR(block_io->ReadBlocks(block_io, block_io->Media->MediaId, 2, read_size, buf))) {
        return 0;
    }
    for (UINTN i = 0; g_robu_partition_name[i]; i++) {
        UINTN off = 56 + i * 2;
        if (off + 1 >= read_size) {
            return 0;
        }
        if (buf[off] != (UINT8)g_robu_partition_name[i] || buf[off + 1] != 0) {
            return 0;
        }
    }
    return 1;
}

EFI_BLOCK_IO_PROTOCOL *efi_find_whole_disk_block_io(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    EFI_BOOT_SERVICES *BS = SystemTable->BootServices;

    EFI_LOADED_IMAGE_PROTOCOL *loaded_image = NULL;
    UINTN esp_prefix_len = 0;
    EFI_DEVICE_PATH_PROTOCOL *esp_path = NULL;
    if (!EFI_ERROR(BS->HandleProtocol(ImageHandle, (EFI_GUID *)&gEfiLoadedImageProtocolGuid,
                                       (void **)&loaded_image)) &&
        loaded_image &&
        !EFI_ERROR(BS->HandleProtocol(loaded_image->DeviceHandle, (EFI_GUID *)&gEfiDevicePathProtocolGuid,
                                       (void **)&esp_path))) {
        esp_prefix_len = dp_prefix_len(esp_path);
    }

    UINTN handle_count = 0;
    EFI_HANDLE *handles = NULL;
    if (EFI_ERROR(BS->LocateHandleBuffer(ByProtocol, (EFI_GUID *)&gEfiBlockIoProtocolGuid, NULL,
                                          &handle_count, &handles))) {
        return NULL;
    }

    EFI_BLOCK_IO_PROTOCOL *dp_match = NULL;
    EFI_BLOCK_IO_PROTOCOL *content_match = NULL;

    for (UINTN i = 0; i < handle_count; i++) {
        EFI_BLOCK_IO_PROTOCOL *block_io = NULL;
        if (EFI_ERROR(BS->HandleProtocol(handles[i], (EFI_GUID *)&gEfiBlockIoProtocolGuid,
                                          (void **)&block_io))) {
            continue;
        }
        if (!block_io->Media || block_io->Media->LogicalPartition) {
            continue;
        }

        if (!dp_match && esp_path) {
            EFI_DEVICE_PATH_PROTOCOL *cand_path = NULL;
            if (!EFI_ERROR(BS->HandleProtocol(handles[i], (EFI_GUID *)&gEfiDevicePathProtocolGuid,
                                               (void **)&cand_path))) {
                UINTN cand_len = dp_full_len_excl_end(cand_path);
                if (cand_len == esp_prefix_len && bytes_equal(cand_path, esp_path, esp_prefix_len)) {
                    dp_match = block_io;
                }
            }
        }

        if (!content_match && is_robu_disk(block_io)) {
            content_match = block_io;
        }
    }

    EFI_BLOCK_IO_PROTOCOL *found = NULL;
    if (dp_match && is_robu_disk(dp_match)) {
        found = dp_match;
    } else if (content_match) {
        found = content_match;
    }

    if (BS->FreePool) {
        BS->FreePool(handles);
    }
    return found;
}
