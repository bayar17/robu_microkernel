#include "efi_types.h"
#include "efi_print.h"
#include "efi_disk.h"
#include "efi_acpi.h"
#include "efi_memmap.h"
#include "efi_kernel_load.h"
#include "ext2_read.h"
#include "mbi_tags.h"

extern void efi_mode_transition_and_jump(unsigned int kernel_entry, unsigned int mbi_addr,
                                         unsigned int transition_addr);
extern UINT8 efi_pm32_template_start[];
extern UINT8 efi_pm32_template_end[];

#define MAX_PHDRS 16
#define MBI_BUF_PAGES 4
#define EFI_TRANSITION_PAGES 1

static UINT8 g_elf_hdr[64];
static UINT8 g_phdrs[MAX_PHDRS * 56];
static char g_cmdline_buf[256];

static EFI_PHYSICAL_ADDRESS g_kernel_temp_addr;
static UINT64 g_kernel_final_addr;
static UINT64 g_kernel_image_size;
static UINT64 g_kernel_entry;

struct bootstrap_module {
    const char *name;
    const char *path;
    EFI_PHYSICAL_ADDRESS addr;
    unsigned int size;
};

static struct bootstrap_module g_bootstrap_modules[] = {
    {"pager", "/boot/bootstrap/pager", 0, 0},
    {"root_task", "/boot/bootstrap/root_task", 0, 0},
    {"devfs", "/boot/bootstrap/devfs", 0, 0},
    {"console_driver", "/boot/bootstrap/console_driver", 0, 0},
    {"ext2fsroot", "/boot/bootstrap/ext2fsroot", 0, 0},
    {"procfs", "/boot/bootstrap/procfs", 0, 0},
    {"sysfs", "/boot/bootstrap/sysfs", 0, 0},
    {"blockdrv", "/boot/bootstrap/blockdrv", 0, 0},
    {"diskfs", "/boot/bootstrap/diskfs", 0, 0},
};

static unsigned int u32_get(const UINT8 *buf, int off) {
    return (unsigned int)buf[off] | ((unsigned int)buf[off + 1] << 8) |
           ((unsigned int)buf[off + 2] << 16) | ((unsigned int)buf[off + 3] << 24);
}

static unsigned short u16_get(const UINT8 *buf, int off) {
    return (unsigned short)buf[off] | ((unsigned short)buf[off + 1] << 8);
}

static void mask_to_pos_size(UINT32 mask, UINT8 *pos, UINT8 *size) {
    UINT8 p = 0, s = 0;
    if (mask == 0) {
        *pos = 0;
        *size = 0;
        return;
    }
    while ((mask & 1) == 0) {
        mask >>= 1;
        p++;
    }
    while (mask & 1) {
        mask >>= 1;
        s++;
    }
    *pos = p;
    *size = s;
}

static int efi_stage_elf_segments(EFI_BOOT_SERVICES *BS, unsigned int ino) {
    if (ext2_read_range(ino, 0, 64, (UINTN)g_elf_hdr) != 0) {
        return -1;
    }
    if (g_elf_hdr[0] != 0x7f || g_elf_hdr[1] != 'E' || g_elf_hdr[2] != 'L' || g_elf_hdr[3] != 'F') {
        return -1;
    }
    if (g_elf_hdr[4] != 2) {
        return -1;
    }
    unsigned int e_entry = u32_get(g_elf_hdr, 24);
    unsigned int e_phoff = u32_get(g_elf_hdr, 32);
    unsigned short e_phentsize = u16_get(g_elf_hdr, 54);
    unsigned short e_phnum = u16_get(g_elf_hdr, 56);
    if (e_phnum > MAX_PHDRS || e_phentsize < 56) {
        return -1;
    }
    if (ext2_read_range(ino, e_phoff, (unsigned int)e_phentsize * e_phnum, (UINTN)g_phdrs) != 0) {
        return -1;
    }

    UINT64 min_addr = 0xFFFFFFFFFFFFFFFFULL;
    UINT64 max_addr = 0;
    int have_load = 0;
    for (int i = 0; i < e_phnum; i++) {
        UINT8 *ph = g_phdrs + i * e_phentsize;
        if (u32_get(ph, 0) != 1) {
            continue;
        }
        unsigned int p_paddr = u32_get(ph, 24);
        unsigned int p_memsz = u32_get(ph, 40);
        if ((UINT64)p_paddr < min_addr) {
            min_addr = p_paddr;
        }
        if ((UINT64)p_paddr + p_memsz > max_addr) {
            max_addr = (UINT64)p_paddr + p_memsz;
        }
        have_load = 1;
    }
    if (!have_load) {
        return -1;
    }

    UINT64 final_start = min_addr & ~0xFFFULL;
    UINT64 final_end = (max_addr + 0xFFF) & ~0xFFFULL;
    UINT64 image_size = final_end - final_start;
    UINTN pages = (UINTN)(image_size / 4096);

    EFI_PHYSICAL_ADDRESS temp_addr = 0;
    if (EFI_ERROR(BS->AllocatePages(AllocateAnyPages, EfiLoaderData, pages, &temp_addr))) {
        return -2;
    }
    UINT8 *temp = (UINT8 *)(UINTN)temp_addr;
    for (UINT64 i = 0; i < image_size; i++) {
        temp[i] = 0;
    }

    for (int i = 0; i < e_phnum; i++) {
        UINT8 *ph = g_phdrs + i * e_phentsize;
        if (u32_get(ph, 0) != 1) {
            continue;
        }
        unsigned int p_offset = u32_get(ph, 8);
        unsigned int p_paddr = u32_get(ph, 24);
        unsigned int p_filesz = u32_get(ph, 32);
        UINT64 rel_off = (UINT64)p_paddr - final_start;
        if (p_filesz > 0) {
            if (ext2_read_range(ino, p_offset, p_filesz, (UINTN)(temp_addr + rel_off)) != 0) {
                return -3;
            }
        }
    }

    g_kernel_temp_addr = temp_addr;
    g_kernel_final_addr = final_start;
    g_kernel_image_size = image_size;
    g_kernel_entry = e_entry;
    return 0;
}

static int efi_load_bootstrap_modules(EFI_BOOT_SERVICES *BS) {
    for (unsigned int i = 0; i < sizeof(g_bootstrap_modules) / sizeof(g_bootstrap_modules[0]); i++) {
        unsigned int ino, size;
        int is_dir;
        if (ext2_resolve_path(g_bootstrap_modules[i].path, &ino, &size, &is_dir) != 0 || is_dir || size == 0) {
            return -1;
        }
        EFI_PHYSICAL_ADDRESS addr = 0x3fffffffULL;
        UINTN pages = (size + 4095) / 4096;
        if (EFI_ERROR(BS->AllocatePages(AllocateMaxAddress, EfiLoaderData, pages, &addr))) {
            return -2;
        }
        if (ext2_read_file(ino, size, (UINTN)addr) != 0) {
            return -3;
        }
        g_bootstrap_modules[i].addr = addr;
        g_bootstrap_modules[i].size = size;
    }
    return 0;
}

void efi_boot_kernel(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    (void)ImageHandle;
    EFI_BOOT_SERVICES *BS = SystemTable->BootServices;

    unsigned int kernel_ino, kernel_size;
    int is_dir;

    if (ext2_resolve_path("/boot/kernel.elf", &kernel_ino, &kernel_size, &is_dir) != 0 || is_dir) {
        efi_print(SystemTable, (CHAR16 *)L"FATAL: could not resolve /boot/kernel.elf\r\n");
        return;
    }

    int elf_rc = efi_stage_elf_segments(BS, kernel_ino);
    if (elf_rc != 0) {
        efi_print(SystemTable, (CHAR16 *)L"FATAL: kernel ELF staging failed, rc=");
        efi_print_hex32(SystemTable, (unsigned int)elf_rc);
        efi_print(SystemTable, (CHAR16 *)L"\r\n");
        return;
    }
    efi_print(SystemTable, (CHAR16 *)L"kernel staged: temp=");
    efi_print_hex64(SystemTable, g_kernel_temp_addr);
    efi_print(SystemTable, (CHAR16 *)L" final=");
    efi_print_hex64(SystemTable, g_kernel_final_addr);
    efi_print(SystemTable, (CHAR16 *)L" size=");
    efi_print_hex64(SystemTable, g_kernel_image_size);
    efi_print(SystemTable, (CHAR16 *)L" entry=");
    efi_print_hex64(SystemTable, g_kernel_entry);
    efi_print(SystemTable, (CHAR16 *)L"\r\n");

    {
        static UINT8 warn_raw_map[16384];
        UINTN warn_map_size = sizeof(warn_raw_map);
        UINTN warn_map_key, warn_desc_size;
        UINT32 warn_desc_ver;
        EFI_STATUS warn_status = BS->GetMemoryMap(&warn_map_size, (EFI_MEMORY_DESCRIPTOR *)warn_raw_map,
                                                   &warn_map_key, &warn_desc_size, &warn_desc_ver);
        if (!EFI_ERROR(warn_status)) {
            UINTN warn_count = warn_map_size / warn_desc_size;
            for (UINTN i = 0; i < warn_count; i++) {
                EFI_MEMORY_DESCRIPTOR *d = (EFI_MEMORY_DESCRIPTOR *)(warn_raw_map + i * warn_desc_size);
                UINT64 d_end = d->PhysicalStart + d->NumberOfPages * 4096ULL;
                int is_risky = d->Type == EfiACPIReclaimMemory || d->Type == EfiACPIMemoryNVS ||
                                d->Type == EfiRuntimeServicesCode || d->Type == EfiRuntimeServicesData ||
                                d->Type == EfiMemoryMappedIO;
                if (is_risky && d->PhysicalStart < g_kernel_final_addr + g_kernel_image_size &&
                    d_end > g_kernel_final_addr) {
                    efi_print(SystemTable, (CHAR16 *)L"WARNING: kernel footprint overlaps live firmware "
                                                       L"memory type=");
                    efi_print_hex32(SystemTable, d->Type);
                    efi_print(SystemTable, (CHAR16 *)L" base=");
                    efi_print_hex64(SystemTable, d->PhysicalStart);
                    efi_print(SystemTable, (CHAR16 *)L" pages=");
                    efi_print_hex64(SystemTable, d->NumberOfPages);
                    efi_print(SystemTable, (CHAR16 *)L"\r\n");
                }
            }
        }
    }

    if (efi_load_bootstrap_modules(BS) != 0) {
        efi_print(SystemTable, (CHAR16 *)L"FATAL: bootstrap module load failed\r\n");
        return;
    }

    const char *cmdline = "root=root_task starter=hello_initsys";
    unsigned int cmdline_ino, cmdline_size;
    if (ext2_resolve_path("/boot/cmdline.txt", &cmdline_ino, &cmdline_size, &is_dir) == 0 && !is_dir &&
        cmdline_size < sizeof(g_cmdline_buf)) {
        if (ext2_read_file(cmdline_ino, cmdline_size, (UINTN)g_cmdline_buf) == 0) {
            g_cmdline_buf[cmdline_size] = '\0';
            while (cmdline_size > 0 &&
                   (g_cmdline_buf[cmdline_size - 1] == '\n' || g_cmdline_buf[cmdline_size - 1] == '\r')) {
                g_cmdline_buf[--cmdline_size] = '\0';
            }
            cmdline = g_cmdline_buf;
        }
    }

    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = NULL;
    int have_fb = 0;
    UINT64 fb_addr = 0;
    unsigned int fb_pitch = 0, fb_width = 0, fb_height = 0;
    UINT8 red_pos = 0, red_size = 0, green_pos = 0, green_size = 0, blue_pos = 0, blue_size = 0;
    if (!EFI_ERROR(BS->LocateProtocol((EFI_GUID *)&gEfiGraphicsOutputProtocolGuid, NULL, (void **)&gop)) &&
        gop && gop->Mode && gop->Mode->Info) {
        EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info = gop->Mode->Info;
        if (info->PixelFormat != PixelBltOnly) {
            fb_addr = gop->Mode->FrameBufferBase;
            fb_width = info->HorizontalResolution;
            fb_height = info->VerticalResolution;
            fb_pitch = info->PixelsPerScanLine * 4;
            if (info->PixelFormat == PixelRedGreenBlueReserved8BitPerColor) {
                red_pos = 0;
                red_size = 8;
                green_pos = 8;
                green_size = 8;
                blue_pos = 16;
                blue_size = 8;
                have_fb = 1;
            } else if (info->PixelFormat == PixelBlueGreenRedReserved8BitPerColor) {
                blue_pos = 0;
                blue_size = 8;
                green_pos = 8;
                green_size = 8;
                red_pos = 16;
                red_size = 8;
                have_fb = 1;
            } else if (info->PixelFormat == PixelBitMask) {
                mask_to_pos_size(info->PixelInformation.RedMask, &red_pos, &red_size);
                mask_to_pos_size(info->PixelInformation.GreenMask, &green_pos, &green_size);
                mask_to_pos_size(info->PixelInformation.BlueMask, &blue_pos, &blue_size);
                have_fb = 1;
            }
        }
    }
    if (have_fb) {
        efi_print(SystemTable, (CHAR16 *)L"GOP: framebuffer found, base=");
        efi_print_hex64(SystemTable, fb_addr);
        efi_print(SystemTable, (CHAR16 *)L" width=");
        efi_print_hex32(SystemTable, fb_width);
        efi_print(SystemTable, (CHAR16 *)L" height=");
        efi_print_hex32(SystemTable, fb_height);
        efi_print(SystemTable, (CHAR16 *)L"\r\n");
    } else {
        efi_print(SystemTable, (CHAR16 *)L"GOP: no usable framebuffer\r\n");
    }

    void *rsdp = efi_find_rsdp(SystemTable);
    if (rsdp) {
        efi_print(SystemTable, (CHAR16 *)L"ACPI: RSDP found at ");
        efi_print_hex64(SystemTable, (unsigned long long)(UINTN)rsdp);
        efi_print(SystemTable, (CHAR16 *)L"\r\n");
    } else {
        efi_print(SystemTable, (CHAR16 *)L"ACPI: RSDP not found\r\n");
    }

    unsigned long long e820_buf = 0;
    unsigned int e820_count = efi_build_e820_map(SystemTable, &e820_buf, NULL);
    efi_print(SystemTable, (CHAR16 *)L"UEFI memmap: entries=");
    efi_print_hex32(SystemTable, e820_count);
    efi_print(SystemTable, (CHAR16 *)L"\r\n");

    UINTN mbi_pages = MBI_BUF_PAGES;
    EFI_PHYSICAL_ADDRESS mbi_mem = 0x3FFFFFFFULL;
    if (EFI_ERROR(BS->AllocatePages(AllocateMaxAddress, EfiLoaderData, mbi_pages, &mbi_mem))) {
        efi_print(SystemTable, (CHAR16 *)L"FATAL: MBI buffer AllocatePages failed\r\n");
        return;
    }
    unsigned int mbi_buf = (unsigned int)mbi_mem;
    unsigned int off = 8;
    off = append_cmdline_tag(mbi_buf, off, cmdline);
    for (unsigned int i = 0; i < sizeof(g_bootstrap_modules) / sizeof(g_bootstrap_modules[0]); i++) {
        unsigned int mod_start = (unsigned int)g_bootstrap_modules[i].addr;
        off = append_module_tag(mbi_buf, off, mod_start, mod_start + g_bootstrap_modules[i].size,
                                g_bootstrap_modules[i].name);
    }

    if (have_fb) {
        off = append_framebuffer_tag(mbi_buf, off, fb_addr, fb_pitch, fb_width, fb_height, 32, red_pos,
                                      red_size, green_pos, green_size, blue_pos, blue_size);
    }
    if (rsdp) {
        off = append_rsdp_tag(mbi_buf, off, (UINTN)rsdp);
    }

    EFI_PHYSICAL_ADDRESS transition_mem = 0x3FFFFFFFULL;
    UINTN transition_size = (UINTN)(efi_pm32_template_end - efi_pm32_template_start);
    if (transition_size == 0 || transition_size > EFI_TRANSITION_PAGES * 4096 ||
        EFI_ERROR(BS->AllocatePages(AllocateMaxAddress, EfiLoaderCode, EFI_TRANSITION_PAGES, &transition_mem))) {
        efi_print(SystemTable, (CHAR16 *)L"FATAL: low EFI transition page allocation failed\r\n");
        return;
    }
    UINT8 *transition_dst = (UINT8 *)(UINTN)transition_mem;
    for (UINTN i = 0; i < transition_size; i++) {
        transition_dst[i] = efi_pm32_template_start[i];
    }

    EFI_STATUS exit_status = 0x8000000000000000ULL;
    unsigned long long final_e820_buf = 0;
    unsigned int final_e820_count = 0;
    for (int attempt = 0; attempt < 5; attempt++) {
        UINTN map_key = 0;
        final_e820_count = efi_build_e820_map(SystemTable, &final_e820_buf, &map_key);
        if (final_e820_count == 0) {
            efi_print(SystemTable, (CHAR16 *)L"FATAL: final memory map fetch failed\r\n");
            return;
        }
        exit_status = BS->ExitBootServices(ImageHandle, map_key);
        if (!EFI_ERROR(exit_status)) {
            break;
        }
    }
    if (EFI_ERROR(exit_status)) {
        efi_print(SystemTable, (CHAR16 *)L"FATAL: ExitBootServices failed after retries, status=");
        efi_print_hex64(SystemTable, (UINT64)exit_status);
        efi_print(SystemTable, (CHAR16 *)L"\r\n");
        return;
    }

    off = append_mmap_tag(mbi_buf, off, (UINTN)final_e820_buf, final_e820_count);
    finalize_mbi(mbi_buf, off);

    UINT8 *ksrc = (UINT8 *)(UINTN)g_kernel_temp_addr;
    UINT8 *kdst = (UINT8 *)(UINTN)g_kernel_final_addr;
    for (UINT64 i = 0; i < g_kernel_image_size; i++) {
        kdst[i] = ksrc[i];
    }

    efi_mode_transition_and_jump((unsigned int)g_kernel_entry, mbi_buf, (unsigned int)transition_mem);
}
