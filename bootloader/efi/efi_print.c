#include "efi_types.h"
#include "efi_print.h"

void efi_print(EFI_SYSTEM_TABLE *SystemTable, const CHAR16 *s) {
    SystemTable->ConOut->OutputString(SystemTable->ConOut, (CHAR16 *)s);
}

void efi_print_hex32(EFI_SYSTEM_TABLE *SystemTable, unsigned int v) {
    CHAR16 buf[11];
    buf[0] = '0';
    buf[1] = 'x';
    buf[10] = 0;
    for (int i = 0; i < 8; i++) {
        unsigned int nibble = (v >> ((7 - i) * 4)) & 0xF;
        buf[2 + i] = (CHAR16)(nibble < 10 ? '0' + nibble : 'a' + (nibble - 10));
    }
    efi_print(SystemTable, buf);
}

void efi_print_hex64(EFI_SYSTEM_TABLE *SystemTable, unsigned long long v) {
    CHAR16 buf[19];
    buf[0] = '0';
    buf[1] = 'x';
    buf[18] = 0;
    for (int i = 0; i < 16; i++) {
        unsigned int nibble = (unsigned int)((v >> ((15 - i) * 4)) & 0xF);
        buf[2 + i] = (CHAR16)(nibble < 10 ? '0' + nibble : 'a' + (nibble - 10));
    }
    efi_print(SystemTable, buf);
}
