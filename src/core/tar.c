#include "robu/tar.h"
#include "robu/arch.h"
#define USTAR_BLOCK 512
typedef struct {
    char name[100];
    char mode[8];
    char uid[8];
    char gid[8];
    char size[12];
    char mtime[12];
    char chksum[8];
    char typeflag;
    char linkname[100];
    char magic[6];
    char version[2];
    char uname[32];
    char gname[32];
    char devmajor[8];
    char devminor[8];
    char prefix[155];
    char pad[12];
} __attribute__((packed)) ustar_header_t;
_Static_assert(sizeof(ustar_header_t) == USTAR_BLOCK,
               "ustar header must be exactly one 512-byte block");
static uint64_t parse_octal(const char *s, int len) {
    int i = 0;
    while (i < len && s[i] == ' ') i++;
    uint64_t v = 0;
    while (i < len && s[i] >= '0' && s[i] <= '7') {
        v = (v << 3) | (uint64_t)(s[i] - '0');
        i++;
    }
    return v;
}
static int is_all_zero(const uint8_t *block) {
    for (int i = 0; i < USTAR_BLOCK; i++) {
        if (block[i] != 0) return 0;
    }
    return 1;
}
static const char *hdr_basename(const char *hdr_name, size_t field_len) {
    const char *base = hdr_name;
    for (size_t i = 0; i < field_len && hdr_name[i]; i++) {
        if (hdr_name[i] == '/' && hdr_name[i + 1] != '\0') {
            base = hdr_name + i + 1;
        }
    }
    return base;
}
int tar_find(const void *archive, uint64_t archive_len, const char *name,
             const uint8_t **out_data, uint64_t *out_size) {
    const uint8_t *base = (const uint8_t *)archive;
    uint64_t off = 0;
    size_t name_len = strlen(name);
    while (off + USTAR_BLOCK <= archive_len) {
        const uint8_t *block = base + off;
        if (is_all_zero(block)) {
            break;
        }
        const ustar_header_t *hdr = (const ustar_header_t *)block;
        if (strncmp(hdr->magic, "ustar", 5) != 0) {
            break;
        }
        uint64_t size = parse_octal(hdr->size, sizeof(hdr->size));
        off += USTAR_BLOCK;
        int is_regular = (hdr->typeflag == '0' || hdr->typeflag == '\0');
        if (is_regular && strncmp(hdr_basename(hdr->name, sizeof(hdr->name)), name, name_len + 1) == 0) {
            *out_data = base + off;
            *out_size = size;
            return 0;
        }
        uint64_t data_blocks = (size + USTAR_BLOCK - 1) / USTAR_BLOCK;
        off += data_blocks * USTAR_BLOCK;
    }
    return -1;
}
int tar_iterate(const void *archive, uint64_t archive_len, uint64_t index,
                 char *name_out, uint64_t name_max, uint64_t *out_size) {
    const uint8_t *base = (const uint8_t *)archive;
    uint64_t off = 0;
    uint64_t seen = 0;
    while (off + USTAR_BLOCK <= archive_len) {
        const uint8_t *block = base + off;
        if (is_all_zero(block)) {
            break;
        }
        const ustar_header_t *hdr = (const ustar_header_t *)block;
        if (strncmp(hdr->magic, "ustar", 5) != 0) {
            break;
        }
        uint64_t size = parse_octal(hdr->size, sizeof(hdr->size));
        off += USTAR_BLOCK;
        int is_regular = (hdr->typeflag == '0' || hdr->typeflag == '\0');
        if (is_regular) {
            if (seen == index) {
                uint64_t i = 0;
                for (; i < name_max - 1 && hdr->name[i]; i++) {
                    name_out[i] = hdr->name[i];
                }
                name_out[i] = '\0';
                *out_size = size;
                return 0;
            }
            seen++;
        }
        uint64_t data_blocks = (size + USTAR_BLOCK - 1) / USTAR_BLOCK;
        off += data_blocks * USTAR_BLOCK;
    }
    return -1;
}
int tar_iterate_all(const void *archive, uint64_t archive_len, uint64_t index,
                     tar_entry_t *out) {
    const uint8_t *base = (const uint8_t *)archive;
    uint64_t off = 0;
    uint64_t seen = 0;
    while (off + USTAR_BLOCK <= archive_len) {
        const uint8_t *block = base + off;
        if (is_all_zero(block)) {
            break;
        }
        const ustar_header_t *hdr = (const ustar_header_t *)block;
        if (strncmp(hdr->magic, "ustar", 5) != 0) {
            break;
        }
        uint64_t size = parse_octal(hdr->size, sizeof(hdr->size));
        off += USTAR_BLOCK;
        int is_regular = (hdr->typeflag == '0' || hdr->typeflag == '\0');
        int is_symlink = (hdr->typeflag == '2');
        if (is_regular || is_symlink) {
            if (seen == index) {
                uint64_t i = 0;
                for (; i < sizeof(out->name) - 1 && hdr->name[i]; i++) {
                    out->name[i] = hdr->name[i];
                }
                out->name[i] = '\0';
                uint64_t j = 0;
                for (; j < sizeof(out->linkname) - 1 && hdr->linkname[j]; j++) {
                    out->linkname[j] = hdr->linkname[j];
                }
                out->linkname[j] = '\0';
                out->type = is_symlink ? TAR_ENTRY_SYMLINK : TAR_ENTRY_FILE;
                out->size = is_symlink ? 0 : size;
                out->data = base + off;
                return 0;
            }
            seen++;
        }
        uint64_t data_blocks = (size + USTAR_BLOCK - 1) / USTAR_BLOCK;
        off += data_blocks * USTAR_BLOCK;
    }
    return -1;
}
