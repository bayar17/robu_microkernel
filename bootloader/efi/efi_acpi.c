#include "efi_types.h"
#include "efi_acpi.h"

static int guid_equal(const EFI_GUID *a, const EFI_GUID *b) {
    const UINT8 *pa = (const UINT8 *)a;
    const UINT8 *pb = (const UINT8 *)b;
    for (int i = 0; i < 16; i++) {
        if (pa[i] != pb[i]) {
            return 0;
        }
    }
    return 1;
}

void *efi_find_rsdp(EFI_SYSTEM_TABLE *SystemTable) {
    void *acpi1 = NULL;
    for (UINTN i = 0; i < SystemTable->NumberOfTableEntries; i++) {
        EFI_CONFIGURATION_TABLE *ct = &SystemTable->ConfigurationTable[i];
        if (guid_equal(&ct->VendorGuid, &gEfiAcpi20TableGuid)) {
            return ct->VendorTable;
        }
        if (guid_equal(&ct->VendorGuid, &gEfiAcpi10TableGuid)) {
            acpi1 = ct->VendorTable;
        }
    }
    return acpi1;
}
