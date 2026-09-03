ARCH ?= x86_64
TARGET := robu_kernel
BUILD_DIR := build
SRC_DIR := src
ARCH_DIR := arch/$(ARCH)
TRACE ?= 0
BUILD_SYS ?= clang

ifeq ($(BUILD_SYS),clang)
  CC := clang
  AS := clang -x assembler-with-cpp
  LD := ld.lld

  STRIP := $(shell command -v llvm-strip 2>/dev/null || echo /opt/homebrew/opt/llvm/bin/llvm-strip)
else ifeq ($(BUILD_SYS),gcc)
  CC := $(ARCH)-elf-gcc
  AS := $(ARCH)-elf-as
  LD := $(ARCH)-elf-ld
  STRIP := $(ARCH)-elf-strip
else
  $(error Unknown BUILD_SYS '$(BUILD_SYS)' -- expected 'clang' or 'gcc')
endif

CFLAGS := -ffreestanding -O2 -g -Wall -Wextra -Iinclude -I$(ARCH_DIR)/include -Iapps/uACPI/include -DUACPI_OVERRIDE_TYPES
CFLAGS += -fno-pic -fno-pie -fno-stack-protector
CFLAGS += -fno-omit-frame-pointer
CFLAGS += -DROBU_TRACE=$(TRACE)

ifeq ($(BUILD_SYS),clang)
	CFLAGS += --target=$(ARCH)-elf
endif

LDFLAGS += -nostdlib -T $(ARCH_DIR)/linker.ld -z noexecstack

ifeq ($(ARCH),x86_64)
	CFLAGS += -mno-red-zone -mno-mmx -mno-sse -mno-sse2
endif
GEN_DIR := $(BUILD_DIR)/generated
CFLAGS += -I$(GEN_DIR)

include src/servers/fs/ext2/Makefile
include src/servers/fs/fat16/Makefile
include src/servers/fs/fat32/Makefile
include src/servers/fs/ramfs/Makefile

RING3_SRC_DIRS := $(SRC_DIR)/servers/%.c $(SRC_DIR)/powertools/%.c
C_SRCS := $(filter-out $(RING3_SRC_DIRS),$(wildcard $(SRC_DIR)/*/*.c)) \
          $(wildcard $(SRC_DIR)/*.c) $(wildcard $(ARCH_DIR)/src/*.c) \
          $(wildcard apps/uACPI/source/*.c)
ASM_SRCS := $(wildcard $(ARCH_DIR)/src/*.S)

OBJS := $(C_SRCS:%.c=$(BUILD_DIR)/%.c.o) $(ASM_SRCS:%.S=$(BUILD_DIR)/%.S.o)

.PHONY: all _all clean mlibc _mlibc busybox run run-gui mlibc-hello linuxvfstest \
        readline _readline bash-configure _bash-configure bash-build _bash-build \
        bootloader run-bootloader bootloader-disk run-bootloader-disk \
        bootloader-kernel-disk run-bootloader-kernel-disk \
	        bootloader-fat32-disk run-bootloader-fat32-disk \
	        bootloader-efi run-bootloader-efi-hello \
	        bootloader-hybrid-disk run-hybrid-disk-bios run-hybrid-disk-efi \
	        ext2-hardening-test

BOOTLOADER_DIR := bootloader
BOOTLOADER_BUILD_DIR := $(BUILD_DIR)/bootloader
BOOTLOADER_ASFLAGS := --target=i386-elf -ffreestanding
BOOTLOADER_CFLAGS := --target=i386-elf -m32 -ffreestanding -fno-pic -fno-pie \
                      -fno-stack-protector -Wall -Wextra -O1
LLVM_OBJCOPY := $(shell command -v llvm-objcopy 2>/dev/null || echo /opt/homebrew/opt/llvm/bin/llvm-objcopy)

$(BOOTLOADER_BUILD_DIR)/stage1.o: $(BOOTLOADER_DIR)/stage1.S
	@mkdir -p $(@D)
	$(AS) $(BOOTLOADER_ASFLAGS) -c $< -o $@

$(BOOTLOADER_BUILD_DIR)/stage1.elf: $(BOOTLOADER_BUILD_DIR)/stage1.o $(BOOTLOADER_DIR)/stage1.ld
	$(LD) -T $(BOOTLOADER_DIR)/stage1.ld -o $@ $(BOOTLOADER_BUILD_DIR)/stage1.o

$(BOOTLOADER_BUILD_DIR)/stage1.bin: $(BOOTLOADER_BUILD_DIR)/stage1.elf
	$(LLVM_OBJCOPY) -O binary $< $@
	@size=$$(stat -f%z $@ 2>/dev/null || stat -c%s $@); \
	if [ "$$size" != "512" ]; then \
	    echo "error: stage1.bin is $$size bytes, expected exactly 512" >&2; \
	    exit 1; \
	fi

$(BOOTLOADER_BUILD_DIR)/stage2.o: $(BOOTLOADER_DIR)/stage2.S
	@mkdir -p $(@D)
	$(AS) $(BOOTLOADER_ASFLAGS) -c $< -o $@

$(BOOTLOADER_BUILD_DIR)/bios_thunk.o: $(BOOTLOADER_DIR)/bios_thunk.S
	@mkdir -p $(@D)
	$(AS) $(BOOTLOADER_ASFLAGS) -c $< -o $@

$(BOOTLOADER_BUILD_DIR)/stage2_main.o: $(BOOTLOADER_DIR)/stage2_main.c $(BOOTLOADER_DIR)/ext2_read.h \
                                        $(BOOTLOADER_DIR)/fat32_read.h
	@mkdir -p $(@D)
	$(CC) $(BOOTLOADER_CFLAGS) -I$(BOOTLOADER_DIR) -c $< -o $@

$(BOOTLOADER_BUILD_DIR)/ext2_read.o: $(BOOTLOADER_DIR)/ext2_read.c $(BOOTLOADER_DIR)/ext2_read.h
	@mkdir -p $(@D)
	$(CC) $(BOOTLOADER_CFLAGS) -I$(BOOTLOADER_DIR) -c $< -o $@

$(BOOTLOADER_BUILD_DIR)/fat32_read.o: $(BOOTLOADER_DIR)/fat32_read.c $(BOOTLOADER_DIR)/fat32_read.h
	@mkdir -p $(@D)
	$(CC) $(BOOTLOADER_CFLAGS) -I$(BOOTLOADER_DIR) -c $< -o $@

$(BOOTLOADER_BUILD_DIR)/kernel_load.o: $(BOOTLOADER_DIR)/kernel_load.c $(BOOTLOADER_DIR)/ext2_read.h \
                                        $(BOOTLOADER_DIR)/mbi_tags.h
	@mkdir -p $(@D)
	$(CC) $(BOOTLOADER_CFLAGS) -I$(BOOTLOADER_DIR) -c $< -o $@

$(BOOTLOADER_BUILD_DIR)/mbi_tags.o: $(BOOTLOADER_DIR)/mbi_tags.c $(BOOTLOADER_DIR)/mbi_tags.h
	@mkdir -p $(@D)
	$(CC) $(BOOTLOADER_CFLAGS) -I$(BOOTLOADER_DIR) -c $< -o $@

$(BOOTLOADER_BUILD_DIR)/jump_kernel.o: $(BOOTLOADER_DIR)/jump_kernel.S
	@mkdir -p $(@D)
	$(AS) $(BOOTLOADER_ASFLAGS) -c $< -o $@

BOOTLOADER_STAGE2_OBJS := $(BOOTLOADER_BUILD_DIR)/stage2.o $(BOOTLOADER_BUILD_DIR)/bios_thunk.o \
                           $(BOOTLOADER_BUILD_DIR)/stage2_main.o $(BOOTLOADER_BUILD_DIR)/ext2_read.o \
                           $(BOOTLOADER_BUILD_DIR)/fat32_read.o \
                           $(BOOTLOADER_BUILD_DIR)/kernel_load.o $(BOOTLOADER_BUILD_DIR)/mbi_tags.o \
                           $(BOOTLOADER_BUILD_DIR)/jump_kernel.o

$(BOOTLOADER_BUILD_DIR)/stage2.elf: $(BOOTLOADER_STAGE2_OBJS) $(BOOTLOADER_DIR)/stage2.ld
	$(LD) -T $(BOOTLOADER_DIR)/stage2.ld -o $@ $(BOOTLOADER_STAGE2_OBJS)

$(BOOTLOADER_BUILD_DIR)/stage2.bin: $(BOOTLOADER_BUILD_DIR)/stage2.elf
	$(LLVM_OBJCOPY) -O binary $< $@
	@size=$$(stat -f%z $@ 2>/dev/null || stat -c%s $@); \
	if [ "$$size" -gt 131072 ]; then \
	    echo "error: stage2.bin is $$size bytes, exceeds the 256-sector (128KiB) budget" >&2; \
	    exit 1; \
	fi

$(BOOTLOADER_BUILD_DIR)/sentinel.bin:
	@mkdir -p $(@D)
	printf 'DEADBEEFCAFEBABE' > $@

BOOTLOADER_DISK_IMG ?= $(BUILD_DIR)/bootloader-stage1.img
BOOTLOADER_SENTINEL_LBA := 300

$(BOOTLOADER_DISK_IMG): $(BOOTLOADER_BUILD_DIR)/stage1.bin $(BOOTLOADER_BUILD_DIR)/stage2.bin \
                         $(BOOTLOADER_BUILD_DIR)/sentinel.bin
	truncate -s 1M $@
	dd if=$(BOOTLOADER_BUILD_DIR)/stage1.bin of=$@ bs=512 count=1 conv=notrunc status=none
	dd if=$(BOOTLOADER_BUILD_DIR)/stage2.bin of=$@ bs=512 seek=34 conv=notrunc status=none
	dd if=$(BOOTLOADER_BUILD_DIR)/sentinel.bin of=$@ bs=512 seek=$(BOOTLOADER_SENTINEL_LBA) conv=notrunc status=none

bootloader: $(BOOTLOADER_DISK_IMG)

run-bootloader: $(BOOTLOADER_DISK_IMG)
	qemu-system-x86_64 -drive file=$(BOOTLOADER_DISK_IMG),format=raw -nographic -no-reboot

EXT2_PARTITION_START_SECTOR := 2048
BOOTLOADER_EXT2_PLACEHOLDER_IMG := $(BOOTLOADER_BUILD_DIR)/ext2-placeholder.img
BOOTLOADER_ROBU_MKE2FS := $(shell command -v mke2fs 2>/dev/null || echo /opt/homebrew/opt/e2fsprogs/sbin/mke2fs)
BOOTLOADER_ROBU_DEBUGFS := $(shell command -v debugfs 2>/dev/null || echo /opt/homebrew/opt/e2fsprogs/sbin/debugfs)

$(BOOTLOADER_BUILD_DIR)/ext2-testfile.txt:
	@mkdir -p $(@D)
	printf 'HELLOFROMEXT2000' > $@

$(BOOTLOADER_BUILD_DIR)/ext2-debugfs-script.txt: $(BOOTLOADER_BUILD_DIR)/ext2-testfile.txt
	@mkdir -p $(@D)
	printf 'mkdir /boot\ncd /boot\nwrite %s test.txt\n' "$(abspath $(BOOTLOADER_BUILD_DIR)/ext2-testfile.txt)" > $@

$(BOOTLOADER_EXT2_PLACEHOLDER_IMG): $(BOOTLOADER_BUILD_DIR)/ext2-debugfs-script.txt
	@mkdir -p $(@D)
	truncate -s 16M $@
	$(BOOTLOADER_ROBU_MKE2FS) -F -t ext2 -b 4096 $@
	printf 'ROBUFSMARK012345' | dd of=$@ bs=1 seek=0 conv=notrunc status=none
	$(BOOTLOADER_ROBU_DEBUGFS) -w -f $(BOOTLOADER_BUILD_DIR)/ext2-debugfs-script.txt $@

BOOTLOADER_EXT2_KERNEL_IMG := $(BOOTLOADER_BUILD_DIR)/ext2-kernel.img
BOOTLOADER_EXT2_KERNEL_SIZE := 96M
BOOTLOADER_EXT2_JOURNAL := $(BOOTLOADER_BUILD_DIR)/robu-journal-v2.bin
BOOTLOADER_EXT2_JOURNAL_BYTES := 1048576
ROOTFS_STAGE := $(BUILD_DIR)/rootfs-stage
ROOTFS_STAGE_STAMP := $(ROOTFS_STAGE)/.stamp
ROOTFS_BOOTSTRAP_NAMES := pager root_task devfs console_driver ext2fsroot procfs sysfs blockdrv diskfs

$(BOOTLOADER_EXT2_JOURNAL):
	@mkdir -p $(@D)
	dd if=/dev/zero bs=$(BOOTLOADER_EXT2_JOURNAL_BYTES) count=1 status=none | tr '\000' 'A' > $@
	dd if=/dev/zero of=$@ bs=512 count=1 conv=notrunc status=none

$(BOOTLOADER_BUILD_DIR)/ext2-kernel-debugfs-script.txt: Makefile $(BUILD_DIR)/$(TARGET) $(ROOTFS_STAGE_STAMP) $(BOOTLOADER_EXT2_JOURNAL)
	@mkdir -p $(@D)
	printf '%s' "$(QEMU_APPEND)" > $(BOOTLOADER_BUILD_DIR)/cmdline.txt
	printf 'mkdir /boot\ncd /boot\nwrite %s kernel.elf\nwrite %s cmdline.txt\nmkdir bootstrap\ncd bootstrap\n' \
	    "$(abspath $(BUILD_DIR)/$(TARGET))" "$(abspath $(BOOTLOADER_BUILD_DIR)/cmdline.txt)" > $@
	for n in $(ROOTFS_BOOTSTRAP_NAMES); do \
	    printf 'write %s %s\n' "$(abspath $(ROOTFS_STAGE))/$$n" "$$n" >> $@; \
	done
	printf 'cd /\nwrite %s .robu-journal\n' "$(abspath $(BOOTLOADER_EXT2_JOURNAL))" >> $@
	python3 scripts/gen-debugfs-userland-script.py $(ROOTFS_STAGE) \
	    $(BOOTLOADER_BUILD_DIR)/ext2-kernel-userland-script.txt
	cat $(BOOTLOADER_BUILD_DIR)/ext2-kernel-userland-script.txt >> $@

$(BOOTLOADER_EXT2_KERNEL_IMG): $(BOOTLOADER_BUILD_DIR)/ext2-kernel-debugfs-script.txt
	@mkdir -p $(@D)
	truncate -s $(BOOTLOADER_EXT2_KERNEL_SIZE) $@
	$(BOOTLOADER_ROBU_MKE2FS) -F -t ext2 -b 4096 $@
	$(BOOTLOADER_ROBU_DEBUGFS) -w -f $(BOOTLOADER_BUILD_DIR)/ext2-kernel-debugfs-script.txt $@

KERNEL_DISK_IMG ?= $(BUILD_DIR)/robu-kernel-disk.img

$(KERNEL_DISK_IMG): $(BOOTLOADER_BUILD_DIR)/stage1.bin $(BOOTLOADER_BUILD_DIR)/stage2.bin \
                     $(BOOTLOADER_EXT2_KERNEL_IMG)
	rm -f $@
	dd if=$(BOOTLOADER_BUILD_DIR)/stage1.bin of=$@ bs=512 count=1 conv=notrunc status=none
	dd if=$(BOOTLOADER_BUILD_DIR)/stage2.bin of=$@ bs=512 seek=34 conv=notrunc status=none
	dd if=$(BOOTLOADER_EXT2_KERNEL_IMG) of=$@ bs=512 seek=$(EXT2_PARTITION_START_SECTOR) conv=notrunc status=none

bootloader-kernel-disk: $(KERNEL_DISK_IMG)

run-bootloader-kernel-disk: $(KERNEL_DISK_IMG)
	@test -f $(QEMU_DISK) || truncate -s 16M $(QEMU_DISK)
	qemu-system-x86_64 -drive file=$(KERNEL_DISK_IMG),format=raw -boot c \
	    -smp 2 -m 512 -nographic -device isa-debug-exit,iobase=0xf4,iosize=0x04 \
	    -drive file=$(QEMU_DISK),format=raw,if=none,id=blk0 \
	    -device virtio-blk-pci,drive=blk0

UNIFIED_DISK_IMG ?= $(BUILD_DIR)/robu-disk.img

$(UNIFIED_DISK_IMG): $(BOOTLOADER_BUILD_DIR)/stage1.bin $(BOOTLOADER_BUILD_DIR)/stage2.bin \
                      $(BOOTLOADER_EXT2_PLACEHOLDER_IMG)
	rm -f $@
	dd if=$(BOOTLOADER_BUILD_DIR)/stage1.bin of=$@ bs=512 count=1 conv=notrunc status=none
	dd if=$(BOOTLOADER_BUILD_DIR)/stage2.bin of=$@ bs=512 seek=34 conv=notrunc status=none
	dd if=$(BOOTLOADER_EXT2_PLACEHOLDER_IMG) of=$@ bs=512 seek=$(EXT2_PARTITION_START_SECTOR) conv=notrunc status=none

bootloader-disk: $(UNIFIED_DISK_IMG)

run-bootloader-disk: $(UNIFIED_DISK_IMG)
	qemu-system-x86_64 -drive file=$(UNIFIED_DISK_IMG),format=raw -nographic -no-reboot

BOOTLOADER_FAT32_PLACEHOLDER_IMG := $(BOOTLOADER_BUILD_DIR)/fat32-placeholder.img
BOOTLOADER_FAT32_TESTFILE := $(BOOTLOADER_BUILD_DIR)/fat32-testfile.txt

$(BOOTLOADER_FAT32_TESTFILE):
	@mkdir -p $(@D)
	printf 'hello fat32 world\n' > $@

$(BOOTLOADER_FAT32_PLACEHOLDER_IMG): $(BOOTLOADER_FAT32_TESTFILE)
	@mkdir -p $(@D)
	rm -f $@
	mformat -C -F -i $@ -v FAT32TEST -T 524288 ::
	mcopy -i $@ $(BOOTLOADER_FAT32_TESTFILE) ::TESTFILE.TXT

FAT32_DISK_IMG ?= $(BUILD_DIR)/robu-fat32-disk.img

$(FAT32_DISK_IMG): $(BOOTLOADER_BUILD_DIR)/stage1.bin $(BOOTLOADER_BUILD_DIR)/stage2.bin \
                    $(BOOTLOADER_FAT32_PLACEHOLDER_IMG)
	rm -f $@
	dd if=$(BOOTLOADER_BUILD_DIR)/stage1.bin of=$@ bs=512 count=1 conv=notrunc status=none
	dd if=$(BOOTLOADER_BUILD_DIR)/stage2.bin of=$@ bs=512 seek=34 conv=notrunc status=none
	dd if=$(BOOTLOADER_FAT32_PLACEHOLDER_IMG) of=$@ bs=512 seek=$(EXT2_PARTITION_START_SECTOR) conv=notrunc status=none

bootloader-fat32-disk: $(FAT32_DISK_IMG)

run-bootloader-fat32-disk: $(FAT32_DISK_IMG)
	qemu-system-x86_64 -drive file=$(FAT32_DISK_IMG),format=raw -nographic -no-reboot

BOOTLOADER_EFI_DIR := $(BOOTLOADER_DIR)/efi
BOOTLOADER_EFI_BUILD_DIR := $(BOOTLOADER_BUILD_DIR)/efi
BOOTLOADER_EFI_CFLAGS := -target x86_64-unknown-windows -ffreestanding -fshort-wchar \
                          -mno-red-zone -Wall -Wextra -Wshorten-64-to-32 -O1
LLD_LINK := $(shell command -v lld-link 2>/dev/null || echo /opt/homebrew/bin/lld-link)

BOOTLOADER_EFI_HEADERS := $(BOOTLOADER_EFI_DIR)/efi_types.h $(BOOTLOADER_EFI_DIR)/efi_disk.h \
                           $(BOOTLOADER_EFI_DIR)/efi_print.h $(BOOTLOADER_EFI_DIR)/efi_acpi.h \
                           $(BOOTLOADER_EFI_DIR)/efi_memmap.h $(BOOTLOADER_EFI_DIR)/efi_kernel_load.h \
                           $(BOOTLOADER_DIR)/ext2_read.h $(BOOTLOADER_DIR)/mbi_tags.h

$(BOOTLOADER_EFI_BUILD_DIR)/%.obj: $(BOOTLOADER_EFI_DIR)/%.c $(BOOTLOADER_EFI_HEADERS)
	@mkdir -p $(@D)
	$(CC) $(BOOTLOADER_EFI_CFLAGS) -I$(BOOTLOADER_EFI_DIR) -I$(BOOTLOADER_DIR) -c $< -o $@

$(BOOTLOADER_EFI_BUILD_DIR)/ext2_read.obj: $(BOOTLOADER_DIR)/ext2_read.c $(BOOTLOADER_DIR)/ext2_read.h
	@mkdir -p $(@D)
	$(CC) $(BOOTLOADER_EFI_CFLAGS) -I$(BOOTLOADER_DIR) -c $< -o $@

$(BOOTLOADER_EFI_BUILD_DIR)/mbi_tags.obj: $(BOOTLOADER_DIR)/mbi_tags.c $(BOOTLOADER_DIR)/mbi_tags.h
	@mkdir -p $(@D)
	$(CC) $(BOOTLOADER_EFI_CFLAGS) -I$(BOOTLOADER_DIR) -c $< -o $@

$(BOOTLOADER_EFI_BUILD_DIR)/efi_mode_transition.obj: $(BOOTLOADER_EFI_DIR)/efi_mode_transition.S
	@mkdir -p $(@D)
	clang -x assembler-with-cpp -target x86_64-unknown-windows -ffreestanding -c $< -o $@

BOOTLOADER_EFI_OBJS := $(BOOTLOADER_EFI_BUILD_DIR)/efi_main.obj $(BOOTLOADER_EFI_BUILD_DIR)/efi_disk_find.obj \
                        $(BOOTLOADER_EFI_BUILD_DIR)/efi_block_io.obj $(BOOTLOADER_EFI_BUILD_DIR)/efi_print.obj \
                        $(BOOTLOADER_EFI_BUILD_DIR)/efi_acpi.obj $(BOOTLOADER_EFI_BUILD_DIR)/efi_memmap.obj \
                        $(BOOTLOADER_EFI_BUILD_DIR)/efi_kernel_load.obj \
                        $(BOOTLOADER_EFI_BUILD_DIR)/efi_mode_transition.obj \
                        $(BOOTLOADER_EFI_BUILD_DIR)/ext2_read.obj $(BOOTLOADER_EFI_BUILD_DIR)/mbi_tags.obj

$(BOOTLOADER_EFI_BUILD_DIR)/BOOTX64.EFI: $(BOOTLOADER_EFI_OBJS)
	$(LLD_LINK) /subsystem:efi_application /entry:efi_main /machine:x64 /nodefaultlib \
	    /out:$@ $(BOOTLOADER_EFI_OBJS)

bootloader-efi: $(BOOTLOADER_EFI_BUILD_DIR)/BOOTX64.EFI

OVMF_CODE := /opt/homebrew/share/qemu/edk2-x86_64-code.fd
OVMF_VARS_TEMPLATE := /opt/homebrew/share/qemu/edk2-i386-vars.fd

run-bootloader-efi-hello: $(BOOTLOADER_EFI_BUILD_DIR)/BOOTX64.EFI
	@test -f $(OVMF_CODE) || (echo "error: OVMF firmware not found at $(OVMF_CODE) -- install/locate it and update OVMF_CODE" >&2 && exit 1)
	@mkdir -p $(BOOTLOADER_EFI_BUILD_DIR)/esp/EFI/BOOT
	cp $(BOOTLOADER_EFI_BUILD_DIR)/BOOTX64.EFI $(BOOTLOADER_EFI_BUILD_DIR)/esp/EFI/BOOT/BOOTX64.EFI
	cp $(OVMF_VARS_TEMPLATE) $(BOOTLOADER_EFI_BUILD_DIR)/ovmf-vars.fd
	qemu-system-x86_64 \
	    -drive if=pflash,format=raw,readonly=on,file=$(OVMF_CODE) \
	    -drive if=pflash,format=raw,file=$(BOOTLOADER_EFI_BUILD_DIR)/ovmf-vars.fd \
	    -drive file=fat:rw:$(BOOTLOADER_EFI_BUILD_DIR)/esp,format=raw -net none -nographic -no-reboot

BOOTLOADER_ESP_SECTORS := 524288
BOOTLOADER_ESP_IMG := $(BOOTLOADER_EFI_BUILD_DIR)/esp.img

$(BOOTLOADER_ESP_IMG): $(BOOTLOADER_EFI_BUILD_DIR)/BOOTX64.EFI
	@mkdir -p $(@D)
	rm -f $@
	mformat -C -F -i $@ -v ROBUESP -T $(BOOTLOADER_ESP_SECTORS) ::
	mmd -i $@ ::EFI
	mmd -i $@ ::EFI/BOOT
	mcopy -i $@ $(BOOTLOADER_EFI_BUILD_DIR)/BOOTX64.EFI ::EFI/BOOT/BOOTX64.EFI

HYBRID_DISK_IMG ?= $(BUILD_DIR)/robu-hybrid-disk.img

$(HYBRID_DISK_IMG): $(BOOTLOADER_BUILD_DIR)/stage1.bin $(BOOTLOADER_BUILD_DIR)/stage2.bin \
                     $(BOOTLOADER_EXT2_KERNEL_IMG) $(BOOTLOADER_ESP_IMG)
	@command -v sgdisk >/dev/null || (echo "error: sgdisk not found -- 'brew install gptfdisk'" >&2 && exit 1)
	rm -f $@
	data_sectors=$$(( $$(stat -f%z $(BOOTLOADER_EXT2_KERNEL_IMG) 2>/dev/null || stat -c%s $(BOOTLOADER_EXT2_KERNEL_IMG)) / 512 )); \
	esp_start=$$(( ( (2048 + $$data_sectors + 2047) / 2048 ) * 2048 )); \
	esp_end=$$(( $$esp_start + $(BOOTLOADER_ESP_SECTORS) - 1 )); \
	total_sectors=$$(( $$esp_end + 1 + 40 )); \
	total_bytes=$$(( $$total_sectors * 512 )); \
	truncate -s $$total_bytes $@; \
	sgdisk -o \
	    -n1:2048:+$$data_sectors -t1:8300 -c1:"robu-root" \
	    -n2:$$esp_start:+$(BOOTLOADER_ESP_SECTORS) -t2:ef00 -c2:"EFI System" \
	    $@; \
	dd if=$(BOOTLOADER_BUILD_DIR)/stage1.bin of=$@ bs=1 count=440 conv=notrunc status=none; \
	dd if=$(BOOTLOADER_BUILD_DIR)/stage2.bin of=$@ bs=512 seek=34 conv=notrunc status=none; \
	dd if=$(BOOTLOADER_EXT2_KERNEL_IMG) of=$@ bs=512 seek=2048 conv=notrunc status=none; \
	dd if=$(BOOTLOADER_ESP_IMG) of=$@ bs=512 seek=$$esp_start conv=notrunc status=none

bootloader-hybrid-disk: $(HYBRID_DISK_IMG)

ext2-hardening-test:
	./scripts/ext2-hardening-test.sh

run-hybrid-disk-bios: $(HYBRID_DISK_IMG)
	@test -f $(QEMU_DISK) || truncate -s 16M $(QEMU_DISK)
	qemu-system-x86_64 -drive file=$(HYBRID_DISK_IMG),format=raw -boot c \
	    -smp 2 -m 512 -nographic -device isa-debug-exit,iobase=0xf4,iosize=0x04 \
	    -drive file=$(QEMU_DISK),format=raw,if=none,id=blk0 \
	    -device virtio-blk-pci,drive=blk0

run-hybrid-disk-efi: $(HYBRID_DISK_IMG)
	@test -f $(OVMF_CODE) || (echo "error: OVMF firmware not found at $(OVMF_CODE) -- install/locate it and update OVMF_CODE" >&2 && exit 1)
	@test -f $(QEMU_DISK) || truncate -s 16M $(QEMU_DISK)
	cp $(OVMF_VARS_TEMPLATE) $(BOOTLOADER_EFI_BUILD_DIR)/ovmf-vars.fd
	qemu-system-x86_64 \
	    -drive if=pflash,format=raw,readonly=on,file=$(OVMF_CODE) \
	    -drive if=pflash,format=raw,file=$(BOOTLOADER_EFI_BUILD_DIR)/ovmf-vars.fd \
	    -drive file=$(HYBRID_DISK_IMG),format=raw \
	    -smp 2 -m 512 -nographic -device isa-debug-exit,iobase=0xf4,iosize=0x04 \
	    -drive file=$(QEMU_DISK),format=raw,if=none,id=blk0 \
	    -device virtio-blk-pci,drive=blk0

all:
	./scripts/identify-os.sh _all

_all: $(BUILD_DIR)/$(TARGET) $(ROOTFS_STAGE_STAMP)

clean:
	rm -rf $(BUILD_DIR)

define GUARD_NATIVE_BUILD
@if [ "$$(uname -s)" != "Linux" ]; then \
    echo "error: kernel compilation is only allowed inside the Docker build container." >&2; \
    echo "Use 'make all' instead of building $@ directly." >&2; \
    exit 1; \
fi
endef

$(BUILD_DIR)/$(TARGET): $(OBJS)
	$(GUARD_NATIVE_BUILD)
	@mkdir -p $(@D)
	$(LD) $(LDFLAGS) -o $@ $(OBJS)

$(BUILD_DIR)/%.c.o: %.c
	$(GUARD_NATIVE_BUILD)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/src/main.c.o:

$(BUILD_DIR)/%.S.o: %.S
	$(GUARD_NATIVE_BUILD)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c $< -o $@

APPS_BUILD_DIR := $(BUILD_DIR)/apps
APP_LDFLAGS := -nostdlib -static -z noexecstack

$(APPS_BUILD_DIR)/devfs/devfs.c.o: src/servers/devfs.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c $< -o $@

$(APPS_BUILD_DIR)/devfs/devfs: $(APPS_BUILD_DIR)/devfs/devfs.c.o $(APP_COMMON_OBJ) apps/link/devfs.ld
	$(LD) $(APP_LDFLAGS) -T apps/link/devfs.ld -e _start -o $@ \
	    $(APPS_BUILD_DIR)/devfs/devfs.c.o $(APP_COMMON_OBJ)

$(APPS_BUILD_DIR)/console_driver/console_driver.c.o: src/servers/console_driver.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c $< -o $@

$(APPS_BUILD_DIR)/console_driver/console_driver: $(APPS_BUILD_DIR)/console_driver/console_driver.c.o $(APP_COMMON_OBJ) apps/link/console_driver.ld
	$(LD) $(APP_LDFLAGS) -T apps/link/console_driver.ld -e _start -o $@ \
	    $(APPS_BUILD_DIR)/console_driver/console_driver.c.o $(APP_COMMON_OBJ)

$(APPS_BUILD_DIR)/blockdrv/blockdrv.c.o: src/servers/blockdrv.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c $< -o $@

$(APPS_BUILD_DIR)/blockdrv/blockdrv: $(APPS_BUILD_DIR)/blockdrv/blockdrv.c.o $(APP_COMMON_OBJ) apps/link/blockdrv.ld
	$(LD) $(APP_LDFLAGS) -T apps/link/blockdrv.ld -e _start -o $@ \
	    $(APPS_BUILD_DIR)/blockdrv/blockdrv.c.o $(APP_COMMON_OBJ)

$(APPS_BUILD_DIR)/fat16fs/block.c.o: $(FAT16FS_DIR)/block.c $(FAT16FS_HDRS)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c $< -o $@

$(APPS_BUILD_DIR)/fat16fs/fat16fs.c.o: $(FAT16FS_DIR)/fat16fs.c $(FAT16FS_INCLUDED_SRCS) $(FAT16FS_HDRS)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c $< -o $@

$(APPS_BUILD_DIR)/fat16fs/fat16fs: $(APPS_BUILD_DIR)/fat16fs/fat16fs.c.o $(APPS_BUILD_DIR)/fat16fs/block.c.o apps/link/fat16fs.ld
	$(LD) $(APP_LDFLAGS) -T apps/link/fat16fs.ld -e _start -o $@ \
	    $(APPS_BUILD_DIR)/fat16fs/fat16fs.c.o $(APPS_BUILD_DIR)/fat16fs/block.c.o

$(APPS_BUILD_DIR)/fat32fs/block.c.o: $(FAT32FS_DIR)/block.c $(FAT32FS_HDRS)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c $< -o $@

$(APPS_BUILD_DIR)/fat32fs/fat32fs.c.o: $(FAT32FS_DIR)/fat32fs.c $(FAT32FS_INCLUDED_SRCS) $(FAT32FS_HDRS)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c $< -o $@

$(APPS_BUILD_DIR)/fat32fs/fat32fs: $(APPS_BUILD_DIR)/fat32fs/fat32fs.c.o $(APPS_BUILD_DIR)/fat32fs/block.c.o apps/link/fat32fs.ld
	$(LD) $(APP_LDFLAGS) -T apps/link/fat32fs.ld -e _start -o $@ \
	    $(APPS_BUILD_DIR)/fat32fs/fat32fs.c.o $(APPS_BUILD_DIR)/fat32fs/block.c.o

$(APPS_BUILD_DIR)/ext2fs/block.c.o: $(EXT2FS_DIR)/block.c $(EXT2FS_HDRS)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c $< -o $@

$(APPS_BUILD_DIR)/ext2fs/ext2fs.c.o: $(EXT2FS_DIR)/ext2fs.c $(EXT2FS_INCLUDED_SRCS) $(EXT2FS_HDRS)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c $< -o $@

$(APPS_BUILD_DIR)/ext2fs/ext2fs: $(APPS_BUILD_DIR)/ext2fs/ext2fs.c.o $(APPS_BUILD_DIR)/ext2fs/block.c.o apps/link/ext2fs.ld
	$(LD) $(APP_LDFLAGS) -T apps/link/ext2fs.ld -e _start -o $@ \
	    $(APPS_BUILD_DIR)/ext2fs/ext2fs.c.o $(APPS_BUILD_DIR)/ext2fs/block.c.o

$(APPS_BUILD_DIR)/ext2fsroot/block.c.o: $(EXT2FS_DIR)/block.c $(EXT2FS_HDRS)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -DEXT2FS_AS_ROOT -c $< -o $@

$(APPS_BUILD_DIR)/ext2fsroot/ext2fs.c.o: $(EXT2FS_DIR)/ext2fs.c $(EXT2FS_INCLUDED_SRCS) $(EXT2FS_HDRS)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -DEXT2FS_AS_ROOT -c $< -o $@

$(APPS_BUILD_DIR)/ext2fsroot/xhci.c.o: src/servers/xhci.c src/servers/xhci.h
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -DEXT2FS_AS_ROOT -c $< -o $@

$(APPS_BUILD_DIR)/ext2fsroot/ext2fsroot: $(APPS_BUILD_DIR)/ext2fsroot/ext2fs.c.o $(APPS_BUILD_DIR)/ext2fsroot/block.c.o $(APPS_BUILD_DIR)/ext2fsroot/xhci.c.o apps/link/ext2fsroot.ld
	$(LD) $(APP_LDFLAGS) -T apps/link/ext2fsroot.ld -e _start -o $@ \
	    $(APPS_BUILD_DIR)/ext2fsroot/ext2fs.c.o $(APPS_BUILD_DIR)/ext2fsroot/block.c.o $(APPS_BUILD_DIR)/ext2fsroot/xhci.c.o

$(APPS_BUILD_DIR)/ramfs/ramfs.c.o: $(RAMFS_DIR)/ramfs.c $(RAMFS_INCLUDED_SRCS) $(RAMFS_HDRS)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c $< -o $@

$(APPS_BUILD_DIR)/ramfs/ramfs: $(APPS_BUILD_DIR)/ramfs/ramfs.c.o $(APP_COMMON_OBJ) apps/link/ramfs.ld
	$(LD) $(APP_LDFLAGS) -T apps/link/ramfs.ld -e _start -o $@ \
	    $(APPS_BUILD_DIR)/ramfs/ramfs.c.o $(APP_COMMON_OBJ)
$(APPS_BUILD_DIR)/procfs/procfs.c.o: src/servers/procfs.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c $< -o $@

$(APPS_BUILD_DIR)/procfs/procfs: $(APPS_BUILD_DIR)/procfs/procfs.c.o $(APP_COMMON_OBJ) apps/link/procfs.ld
	$(LD) $(APP_LDFLAGS) -T apps/link/procfs.ld -e _start -o $@ \
	    $(APPS_BUILD_DIR)/procfs/procfs.c.o $(APP_COMMON_OBJ)
$(APPS_BUILD_DIR)/sysfs/sysfs.c.o: src/servers/sysfs.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c $< -o $@

$(APPS_BUILD_DIR)/sysfs/sysfs: $(APPS_BUILD_DIR)/sysfs/sysfs.c.o $(APP_COMMON_OBJ) apps/link/sysfs.ld
	$(LD) $(APP_LDFLAGS) -T apps/link/sysfs.ld -e _start -o $@ \
	    $(APPS_BUILD_DIR)/sysfs/sysfs.c.o $(APP_COMMON_OBJ)

$(APPS_BUILD_DIR)/bootfs/bootfs.c.o: src/servers/bootfs.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c $< -o $@

$(APPS_BUILD_DIR)/bootfs/bootfs: $(APPS_BUILD_DIR)/bootfs/bootfs.c.o $(APP_COMMON_OBJ) apps/link/bootfs.ld
	$(LD) $(APP_LDFLAGS) -T apps/link/bootfs.ld -e _start -o $@ \
	    $(APPS_BUILD_DIR)/bootfs/bootfs.c.o $(APP_COMMON_OBJ)

$(APPS_BUILD_DIR)/diskfs/diskfs.c.o: src/servers/diskfs.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c $< -o $@

$(APPS_BUILD_DIR)/diskfs/diskfs: $(APPS_BUILD_DIR)/diskfs/diskfs.c.o $(APP_COMMON_OBJ) apps/link/diskfs.ld
	$(LD) $(APP_LDFLAGS) -T apps/link/diskfs.ld -e _start -o $@ \
	    $(APPS_BUILD_DIR)/diskfs/diskfs.c.o $(APP_COMMON_OBJ)

$(APPS_BUILD_DIR)/pager/pager.c.o: src/servers/pager.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c $< -o $@

$(APPS_BUILD_DIR)/pager/pager: $(APPS_BUILD_DIR)/pager/pager.c.o $(APP_COMMON_OBJ) apps/link/pager.ld
	$(LD) $(APP_LDFLAGS) -T apps/link/pager.ld -e _start -o $@ \
	    $(APPS_BUILD_DIR)/pager/pager.c.o $(APP_COMMON_OBJ)

$(APPS_BUILD_DIR)/powertools/reboot.c.o: src/powertools/reboot.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c $< -o $@

$(APPS_BUILD_DIR)/powertools/reboot: $(APPS_BUILD_DIR)/powertools/reboot.c.o apps/link/reboot.ld
	$(LD) $(APP_LDFLAGS) -T apps/link/reboot.ld -e _start -o $@ \
	    $(APPS_BUILD_DIR)/powertools/reboot.c.o

$(APPS_BUILD_DIR)/powertools/halt.c.o: src/powertools/halt.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c $< -o $@

$(APPS_BUILD_DIR)/powertools/halt: $(APPS_BUILD_DIR)/powertools/halt.c.o apps/link/halt.ld
	$(LD) $(APP_LDFLAGS) -T apps/link/halt.ld -e _start -o $@ \
	    $(APPS_BUILD_DIR)/powertools/halt.c.o

$(APPS_BUILD_DIR)/powertools/shutdown.c.o: src/powertools/shutdown.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c $< -o $@

$(APPS_BUILD_DIR)/powertools/shutdown: $(APPS_BUILD_DIR)/powertools/shutdown.c.o apps/link/shutdown.ld
	$(LD) $(APP_LDFLAGS) -T apps/link/shutdown.ld -e _start -o $@ \
	    $(APPS_BUILD_DIR)/powertools/shutdown.c.o

$(APPS_BUILD_DIR)/sigtest/sigtest.c.o: tests/sigtest/sigtest.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c $< -o $@

$(APPS_BUILD_DIR)/sigtest/sigtest: $(APPS_BUILD_DIR)/sigtest/sigtest.c.o apps/link/sigtest.ld
	$(LD) $(APP_LDFLAGS) -T apps/link/sigtest.ld -e _start -o $@ \
	    $(APPS_BUILD_DIR)/sigtest/sigtest.c.o

$(APPS_BUILD_DIR)/consoletest/consoletest.c.o: tests/consoletest/consoletest.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c $< -o $@

$(APPS_BUILD_DIR)/consoletest/consoletest: $(APPS_BUILD_DIR)/consoletest/consoletest.c.o apps/link/consoletest.ld
	$(LD) $(APP_LDFLAGS) -T apps/link/consoletest.ld -e _start -o $@ \
	    $(APPS_BUILD_DIR)/consoletest/consoletest.c.o

$(APPS_BUILD_DIR)/mousetest/mousetest.c.o: tests/mousetest/mousetest.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c $< -o $@

$(APPS_BUILD_DIR)/mousetest/mousetest: $(APPS_BUILD_DIR)/mousetest/mousetest.c.o apps/link/mousetest.ld
	$(LD) $(APP_LDFLAGS) -T apps/link/mousetest.ld -e _start -o $@ \
	    $(APPS_BUILD_DIR)/mousetest/mousetest.c.o

$(APPS_BUILD_DIR)/fbtest/fbtest.c.o: tests/fbtest/fbtest.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c $< -o $@

$(APPS_BUILD_DIR)/fbtest/fbtest: $(APPS_BUILD_DIR)/fbtest/fbtest.c.o apps/link/fbtest.ld
	$(LD) $(APP_LDFLAGS) -T apps/link/fbtest.ld -e _start -o $@ \
	    $(APPS_BUILD_DIR)/fbtest/fbtest.c.o

$(APPS_BUILD_DIR)/pcitest/pcitest.c.o: tests/pcitest/pcitest.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c $< -o $@

$(APPS_BUILD_DIR)/pcitest/pcitest: $(APPS_BUILD_DIR)/pcitest/pcitest.c.o apps/link/pcitest.ld
	$(LD) $(APP_LDFLAGS) -T apps/link/pcitest.ld -e _start -o $@ \
	    $(APPS_BUILD_DIR)/pcitest/pcitest.c.o

$(APPS_BUILD_DIR)/diskfstest_write/diskfstest_write.c.o: tests/diskfstest/diskfstest_write.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c $< -o $@

$(APPS_BUILD_DIR)/diskfstest_write/diskfstest_write: $(APPS_BUILD_DIR)/diskfstest_write/diskfstest_write.c.o apps/link/diskfstest_write.ld
	$(LD) $(APP_LDFLAGS) -T apps/link/diskfstest_write.ld -e _start -o $@ \
	    $(APPS_BUILD_DIR)/diskfstest_write/diskfstest_write.c.o

$(APPS_BUILD_DIR)/diskfstest_verify/diskfstest_verify.c.o: tests/diskfstest/diskfstest_verify.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c $< -o $@

$(APPS_BUILD_DIR)/diskfstest_verify/diskfstest_verify: $(APPS_BUILD_DIR)/diskfstest_verify/diskfstest_verify.c.o apps/link/diskfstest_verify.ld
	$(LD) $(APP_LDFLAGS) -T apps/link/diskfstest_verify.ld -e _start -o $@ \
	    $(APPS_BUILD_DIR)/diskfstest_verify/diskfstest_verify.c.o

$(APPS_BUILD_DIR)/fat16fstest/fat16fstest.c.o: tests/fat16fstest/fat16fstest.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c $< -o $@

$(APPS_BUILD_DIR)/fat16fstest/fat16fstest: $(APPS_BUILD_DIR)/fat16fstest/fat16fstest.c.o apps/link/fat16fstest.ld
	$(LD) $(APP_LDFLAGS) -T apps/link/fat16fstest.ld -e _start -o $@ \
	    $(APPS_BUILD_DIR)/fat16fstest/fat16fstest.c.o

$(APPS_BUILD_DIR)/fat16fstest_write/fat16fstest_write.c.o: tests/fat16fstest/fat16fstest_write.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c $< -o $@

$(APPS_BUILD_DIR)/fat16fstest_write/fat16fstest_write: $(APPS_BUILD_DIR)/fat16fstest_write/fat16fstest_write.c.o apps/link/fat16fstest_write.ld
	$(LD) $(APP_LDFLAGS) -T apps/link/fat16fstest_write.ld -e _start -o $@ \
	    $(APPS_BUILD_DIR)/fat16fstest_write/fat16fstest_write.c.o

$(APPS_BUILD_DIR)/fat32fstest/fat32fstest.c.o: tests/fat32fstest/fat32fstest.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c $< -o $@

$(APPS_BUILD_DIR)/fat32fstest/fat32fstest: $(APPS_BUILD_DIR)/fat32fstest/fat32fstest.c.o apps/link/fat32fstest.ld
	$(LD) $(APP_LDFLAGS) -T apps/link/fat32fstest.ld -e _start -o $@ \
	    $(APPS_BUILD_DIR)/fat32fstest/fat32fstest.c.o

$(APPS_BUILD_DIR)/ext2fstest/ext2fstest.c.o: tests/ext2fstest/ext2fstest.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c $< -o $@

$(APPS_BUILD_DIR)/ext2fstest/ext2fstest: $(APPS_BUILD_DIR)/ext2fstest/ext2fstest.c.o apps/link/ext2fstest.ld
	$(LD) $(APP_LDFLAGS) -T apps/link/ext2fstest.ld -e _start -o $@ \
	    $(APPS_BUILD_DIR)/ext2fstest/ext2fstest.c.o

$(APPS_BUILD_DIR)/shmtest_producer/shmtest_producer.c.o: tests/shmtest/shmtest_producer.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c $< -o $@

$(APPS_BUILD_DIR)/shmtest_producer/shmtest_producer: $(APPS_BUILD_DIR)/shmtest_producer/shmtest_producer.c.o apps/link/shmtest_producer.ld
	$(LD) $(APP_LDFLAGS) -T apps/link/shmtest_producer.ld -e _start -o $@ \
	    $(APPS_BUILD_DIR)/shmtest_producer/shmtest_producer.c.o

$(APPS_BUILD_DIR)/shmtest_consumer/shmtest_consumer.c.o: tests/shmtest/shmtest_consumer.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c $< -o $@

$(APPS_BUILD_DIR)/shmtest_consumer/shmtest_consumer: $(APPS_BUILD_DIR)/shmtest_consumer/shmtest_consumer.c.o apps/link/shmtest_consumer.ld
	$(LD) $(APP_LDFLAGS) -T apps/link/shmtest_consumer.ld -e _start -o $@ \
	    $(APPS_BUILD_DIR)/shmtest_consumer/shmtest_consumer.c.o

$(APPS_BUILD_DIR)/socktest_server/socktest_server.c.o: tests/socktest/socktest_server.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c $< -o $@

$(APPS_BUILD_DIR)/socktest_server/socktest_server: $(APPS_BUILD_DIR)/socktest_server/socktest_server.c.o apps/link/socktest_server.ld
	$(LD) $(APP_LDFLAGS) -T apps/link/socktest_server.ld -e _start -o $@ \
	    $(APPS_BUILD_DIR)/socktest_server/socktest_server.c.o

$(APPS_BUILD_DIR)/socktest_client/socktest_client.c.o: tests/socktest/socktest_client.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c $< -o $@

$(APPS_BUILD_DIR)/socktest_client/socktest_client: $(APPS_BUILD_DIR)/socktest_client/socktest_client.c.o apps/link/socktest_client.ld
	$(LD) $(APP_LDFLAGS) -T apps/link/socktest_client.ld -e _start -o $@ \
	    $(APPS_BUILD_DIR)/socktest_client/socktest_client.c.o

$(APPS_BUILD_DIR)/vfs_mount/mount_devfs.c.o: apps/vfs_mount/mount_devfs.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c $< -o $@

$(APPS_BUILD_DIR)/vfs_mount/mount_devfs: $(APPS_BUILD_DIR)/vfs_mount/mount_devfs.c.o apps/link/mount_devfs.ld
	$(LD) $(APP_LDFLAGS) -T apps/link/mount_devfs.ld -e _start -o $@ \
	    $(APPS_BUILD_DIR)/vfs_mount/mount_devfs.c.o

$(APPS_BUILD_DIR)/vfs_mount/mount_procfs.c.o: apps/vfs_mount/mount_procfs.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c $< -o $@

$(APPS_BUILD_DIR)/vfs_mount/mount_procfs: $(APPS_BUILD_DIR)/vfs_mount/mount_procfs.c.o apps/link/mount_procfs.ld
	$(LD) $(APP_LDFLAGS) -T apps/link/mount_procfs.ld -e _start -o $@ \
	    $(APPS_BUILD_DIR)/vfs_mount/mount_procfs.c.o

$(APPS_BUILD_DIR)/vfs_mount/mount_sysfs.c.o: apps/vfs_mount/mount_sysfs.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c $< -o $@

$(APPS_BUILD_DIR)/vfs_mount/mount_sysfs: $(APPS_BUILD_DIR)/vfs_mount/mount_sysfs.c.o apps/link/mount_sysfs.ld
	$(LD) $(APP_LDFLAGS) -T apps/link/mount_sysfs.ld -e _start -o $@ \
	    $(APPS_BUILD_DIR)/vfs_mount/mount_sysfs.c.o

$(APPS_BUILD_DIR)/vfs_mount/mount_tmpfs.c.o: apps/vfs_mount/mount_tmpfs.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c $< -o $@

$(APPS_BUILD_DIR)/vfs_mount/mount_tmpfs: $(APPS_BUILD_DIR)/vfs_mount/mount_tmpfs.c.o apps/link/mount_tmpfs.ld
	$(LD) $(APP_LDFLAGS) -T apps/link/mount_tmpfs.ld -e _start -o $@ \
	    $(APPS_BUILD_DIR)/vfs_mount/mount_tmpfs.c.o

MLIBC_DIR := apps/mlibc
MLIBC_BUILD_DIR := $(BUILD_DIR)/mlibc
MLIBC_SYSROOT := $(abspath $(BUILD_DIR)/mlibc-sysroot)
MLIBC_CROSS := apps/mlibc-robu-cross.ini
MLIBC_TOOLCHAIN_PATH := /opt/homebrew/opt/llvm/bin:/opt/homebrew/opt/bison/bin:$(PATH)

empty :=
space := $(empty) $(empty)
mlibc:
	./scripts/identify-os.sh _mlibc

_mlibc:
	@if [ ! -f $(MLIBC_BUILD_DIR)/build.ninja ]; then \
	    PATH="$(MLIBC_TOOLCHAIN_PATH)" meson setup $(MLIBC_BUILD_DIR) $(MLIBC_DIR) --cross-file $(MLIBC_CROSS) \
	        -Dlibgcc_dependency=false -Ddefault_library=static --prefix=/usr; \
	else \
	    PATH="$(MLIBC_TOOLCHAIN_PATH)" meson configure $(MLIBC_BUILD_DIR); \
	fi
	PATH="$(MLIBC_TOOLCHAIN_PATH)" ninja -C $(MLIBC_BUILD_DIR)
	DESTDIR=$(MLIBC_SYSROOT) PATH="$(MLIBC_TOOLCHAIN_PATH)" ninja -C $(MLIBC_BUILD_DIR) install

MLIBC_APP_CFLAGS := --target=x86_64-linux-gnu -ffreestanding -fPIC -fno-stack-protector \
                     -mno-red-zone -D_GNU_SOURCE -Wall -Wextra \
                     -isystem $(MLIBC_SYSROOT)/usr/include

MLIBC_CRT_OBJS := $(MLIBC_SYSROOT)/usr/lib/crt1.o
MLIBC_LIBS := --start-group $(MLIBC_SYSROOT)/usr/lib/libc.a $(MLIBC_SYSROOT)/usr/lib/libm.a --end-group
CONFUSE_DIR := apps/confuse/src
CONFUSE_BUILD_DIR := $(APPS_BUILD_DIR)/confuse
CONFUSE_CFLAGS := $(MLIBC_APP_CFLAGS) -I$(CONFUSE_DIR) \
                   -DHAVE_UNISTD_H -DHAVE_SYS_STAT_H -DHAVE_STRING_H \
                   -DHAVE_STRDUP -DHAVE_STRNDUP -DHAVE_STRCASECMP -DHAVE_FMEMOPEN \
                   -DPACKAGE=\"libconfuse\" -DPACKAGE_VERSION=\"3.3\" -DPACKAGE_STRING=\"libconfuse-3.3\" \
                   -DCONFUSE_NO_FLOAT \
                   -Wno-unused-parameter -Wno-unused-function -Wno-unused-variable -Wno-sign-compare

$(CONFUSE_BUILD_DIR)/lexer.c: $(CONFUSE_DIR)/lexer.l
	@mkdir -p $(@D)
	flex -Pcfg_yy -o $@ $<

$(CONFUSE_BUILD_DIR)/lexer.c.o: $(CONFUSE_BUILD_DIR)/lexer.c
	@mkdir -p $(@D)
	$(CC) $(CONFUSE_CFLAGS) -Wno-unused-but-set-variable -c $< -o $@

$(CONFUSE_BUILD_DIR)/confuse.c.o: $(CONFUSE_DIR)/confuse.c
	@mkdir -p $(@D)
	$(CC) $(CONFUSE_CFLAGS) -c $< -o $@

$(CONFUSE_BUILD_DIR)/reallocarray.c.o: $(CONFUSE_DIR)/reallocarray.c
	@mkdir -p $(@D)
	$(CC) $(CONFUSE_CFLAGS) -c $< -o $@

CONFUSE_OBJS := $(CONFUSE_BUILD_DIR)/confuse.c.o $(CONFUSE_BUILD_DIR)/lexer.c.o $(CONFUSE_BUILD_DIR)/reallocarray.c.o

$(APPS_BUILD_DIR)/hello_initsys/main.c.o: apps/hello_initsys/main.c mlibc
	@mkdir -p $(@D)
	$(CC) $(MLIBC_APP_CFLAGS) -I$(CONFUSE_DIR) -c $< -o $@

$(APPS_BUILD_DIR)/hello_initsys/hello_initsys: $(APPS_BUILD_DIR)/hello_initsys/main.c.o $(CONFUSE_OBJS) apps/link/hello_initsys.ld
	ld.lld -nostdlib -static -T apps/link/hello_initsys.ld -e _start -o $@ \
	    $(MLIBC_CRT_OBJS) $(APPS_BUILD_DIR)/hello_initsys/main.c.o $(CONFUSE_OBJS) $(MLIBC_LIBS)
	$(STRIP) --strip-all $@
$(APPS_BUILD_DIR)/tty_service/tty_service.c.o: apps/tty_service/tty_service.c mlibc
	@mkdir -p $(@D)
	$(CC) $(MLIBC_APP_CFLAGS) -I$(CONFUSE_DIR) -c $< -o $@

$(APPS_BUILD_DIR)/tty_service/tty_service: $(APPS_BUILD_DIR)/tty_service/tty_service.c.o $(CONFUSE_OBJS) apps/link/tty_service.ld
	ld.lld -nostdlib -static -T apps/link/tty_service.ld -e _start -o $@ \
	    $(MLIBC_CRT_OBJS) $(APPS_BUILD_DIR)/tty_service/tty_service.c.o $(CONFUSE_OBJS) $(MLIBC_LIBS)
	$(STRIP) --strip-all $@

$(APPS_BUILD_DIR)/mlibc-hello/hello.c.o: tests/mlibc-hello/hello.c mlibc
	@mkdir -p $(@D)
	$(CC) $(MLIBC_APP_CFLAGS) -c $< -o $@

$(APPS_BUILD_DIR)/mlibc-hello/hello: $(APPS_BUILD_DIR)/mlibc-hello/hello.c.o apps/link/mlibchello.ld
	ld.lld -nostdlib -static -T apps/link/mlibchello.ld -e _start -o $@ \
	    $(MLIBC_CRT_OBJS) $(APPS_BUILD_DIR)/mlibc-hello/hello.c.o $(MLIBC_LIBS)
	$(STRIP) --strip-all $@

mlibc-hello: $(APPS_BUILD_DIR)/mlibc-hello/hello

$(APPS_BUILD_DIR)/linuxvfstest/linuxvfstest.c.o: tests/linuxvfstest/linuxvfstest.c mlibc
	@mkdir -p $(@D)
	$(CC) $(MLIBC_APP_CFLAGS) -c $< -o $@

$(APPS_BUILD_DIR)/linuxvfstest/linuxvfstest: $(APPS_BUILD_DIR)/linuxvfstest/linuxvfstest.c.o apps/link/mlibchello.ld
	ld.lld -nostdlib -static -T apps/link/mlibchello.ld -e _start -o $@ \
	    $(MLIBC_CRT_OBJS) $(APPS_BUILD_DIR)/linuxvfstest/linuxvfstest.c.o $(MLIBC_LIBS)
	$(STRIP) --strip-all $@

linuxvfstest: $(APPS_BUILD_DIR)/linuxvfstest/linuxvfstest

AR := $(shell command -v llvm-ar 2>/dev/null || echo /opt/homebrew/opt/llvm/bin/llvm-ar)

BUSYBOX_DIR := apps/busybox
BUSYBOX_ROBU_DIR := apps/busybox-robu
BUSYBOX_BUILD_DIR := $(APPS_BUILD_DIR)/busybox
BUSYBOX_BUILD_SOURCE := $(BUSYBOX_BUILD_DIR)/source
BUSYBOX_BIN := $(BUSYBOX_BUILD_DIR)/busybox
BUSYBOX_HOSTCC ?= clang
BUSYBOX_ROBU_FILES := $(BUSYBOX_ROBU_DIR)/malloc.h $(BUSYBOX_ROBU_DIR)/mntent.h \
                      $(BUSYBOX_ROBU_DIR)/sched.h $(BUSYBOX_ROBU_DIR)/sys/statfs.h \
	                  $(BUSYBOX_ROBU_DIR)/sys/sysmacros.h $(BUSYBOX_ROBU_DIR)/sys/mount.h \
	                  $(BUSYBOX_ROBU_DIR)/robu-sysinfo-include.h \
	                  $(BUSYBOX_ROBU_DIR)/disable-pidof-exe.patch
BUSYBOX_CFLAGS := -fuse-ld=lld $(MLIBC_APP_CFLAGS) -D__robu__ -U__linux__ \
	              -I$(abspath $(BUSYBOX_ROBU_DIR)) -include robu-sysinfo-include.h
BUSYBOX_LDFLAGS := -nostdlib -static -Wl,-T,$(abspath apps/link/busybox.ld) \
                   -Wl,-e,_start -Wl,-u,main
BUSYBOX_LDLIBS := -L$(MLIBC_SYSROOT)/usr/lib -l:crt1.o -lc -lm

$(BUSYBOX_BUILD_SOURCE)/.config: $(BUSYBOX_DIR)/Config.in $(BUSYBOX_DIR)/Makefile \
                                  $(BUSYBOX_ROBU_FILES) scripts/configure-busybox.sh
	rm -rf $(BUSYBOX_BUILD_SOURCE)
	@mkdir -p $(BUSYBOX_BUILD_SOURCE)
	git -C $(BUSYBOX_DIR) archive --format=tar HEAD | tar -xf - -C $(BUSYBOX_BUILD_SOURCE)
	HOSTCC="$(BUSYBOX_HOSTCC)" ./scripts/configure-busybox.sh $(BUSYBOX_BUILD_SOURCE)

$(BUSYBOX_BIN): $(BUSYBOX_BUILD_SOURCE)/.config mlibc apps/link/busybox.ld
	@mkdir -p $(@D)
	$(MAKE) -C $(BUSYBOX_BUILD_SOURCE) busybox \
	    HOSTCC="$(BUSYBOX_HOSTCC)" CC="$(CC) $(BUSYBOX_CFLAGS)" LD="$(LD)" AR="$(AR)" STRIP="$(STRIP)" \
	    CONFIG_EXTRA_LDFLAGS="$(BUSYBOX_LDFLAGS)" \
	    CONFIG_EXTRA_LDLIBS="$(BUSYBOX_LDLIBS)"
	cp $(BUSYBOX_BUILD_SOURCE)/busybox $@

busybox: $(BUSYBOX_BIN)

READLINE_DIR := apps/readline
READLINE_BUILD_DIR := $(APPS_BUILD_DIR)/readline
READLINE_CFLAGS := $(MLIBC_APP_CFLAGS) -I$(READLINE_DIR) -DHAVE_CONFIG_H \
                    -DRL_LIBRARY_VERSION=\"8.3\" \
                    -Wno-unused-function -Wno-unused-parameter -Wno-unused-variable \
                    -Wno-unused-but-set-variable -Wno-sign-compare -Wno-implicit-fallthrough

READLINE_SRCS := readline.c funmap.c keymaps.c vi_mode.c parens.c rltty.c \
                  complete.c bind.c isearch.c display.c signals.c emacs_keymap.c \
                  vi_keymap.c util.c kill.c undo.c macro.c input.c \
                  callback.c terminal.c xmalloc.c xfree.c \
                  history.c histsearch.c histexpand.c histfile.c nls.c search.c \
                  shell.c savestring.c tilde.c text.c misc.c compat.c \
                  mbutil.c gettimeofday.c colors.c parse-colors.c \
                  robu-termcap.c

READLINE_OBJS := $(patsubst %.c,$(READLINE_BUILD_DIR)/%.c.o,$(READLINE_SRCS))

$(READLINE_OBJS): $(READLINE_BUILD_DIR)/%.c.o: $(READLINE_DIR)/%.c mlibc
	@mkdir -p $(@D)
	@[ -L $(READLINE_DIR)/readline ] || ln -s . $(READLINE_DIR)/readline
	$(CC) $(READLINE_CFLAGS) -c $< -o $@

$(READLINE_BUILD_DIR)/libreadline.a: $(READLINE_OBJS)
	$(AR) rcs $@ $(READLINE_OBJS)

READLINE_PREFIX := $(abspath $(BUILD_DIR)/readline-prefix)

$(READLINE_PREFIX)/lib/libreadline.a: $(READLINE_BUILD_DIR)/libreadline.a
	@mkdir -p $(READLINE_PREFIX)/lib $(READLINE_PREFIX)/include
	@ln -sf $(abspath $(READLINE_BUILD_DIR)/libreadline.a) $(READLINE_PREFIX)/lib/libreadline.a
	@ln -sf $(abspath $(READLINE_BUILD_DIR)/libreadline.a) $(READLINE_PREFIX)/lib/libhistory.a
	@[ -L $(READLINE_PREFIX)/include/readline ] || ln -sf $(abspath $(READLINE_DIR)) $(READLINE_PREFIX)/include/readline

readline:
	./scripts/identify-os.sh _readline

_readline: $(READLINE_BUILD_DIR)/libreadline.a

BASH_DIR := apps/bash
BASH_BUILD_DIR := $(APPS_BUILD_DIR)/bash

$(BASH_DIR)/config.h: mlibc readline $(READLINE_PREFIX)/lib/libreadline.a
	cd $(BASH_DIR) && CC="clang --target=x86_64-linux-gnu" \
	    CC_FOR_BUILD=clang \
	    CFLAGS="-ffreestanding -fPIC -fno-stack-protector -mno-red-zone -D_GNU_SOURCE -nostdinc -isystem $$(clang --print-resource-dir)/include -isystem $(MLIBC_SYSROOT)/usr/include" \
	    CPPFLAGS="-I$(abspath $(READLINE_DIR))" \
	    ./configure --host=x86_64-linux-gnu --build=x86_64-pc-linux-gnu \
	        --cache-file=$(abspath $(BASH_DIR)/robu.cache) \
	        --without-bash-malloc --enable-readline --enable-job-control \
	        --with-installed-readline=$(READLINE_PREFIX) \
	        --disable-nls --disable-rpath --without-libiconv-prefix --without-libintl-prefix

bash-configure:
	./scripts/identify-os.sh _bash-configure

_bash-configure: $(BASH_DIR)/config.h

bash-build:
	./scripts/identify-os.sh _bash-build

_bash-build: $(BASH_DIR)/config.h
	touch $(BASH_DIR)/configure.ac $(BASH_DIR)/aclocal.m4 $(BASH_DIR)/config.h.in
	touch $(BASH_DIR)/configure
	touch $(BASH_DIR)/config.status $(BASH_DIR)/Makefile
	./scripts/bash-patch-makefile.sh $(BASH_DIR)/Makefile $(abspath apps/link/bash.ld) \
	    $(MLIBC_CRT_OBJS) $(MLIBC_SYSROOT)/usr/lib/libc.a $(MLIBC_SYSROOT)/usr/lib/libm.a
	$(MAKE) -C $(BASH_DIR)

$(BASH_DIR)/bash:
	$(MAKE) bash-build

$(APPS_BUILD_DIR)/readlinetest/readlinetest.c.o: tests/readlinetest/readlinetest.c mlibc
	@mkdir -p $(@D)
	@[ -L $(READLINE_DIR)/readline ] || ln -s . $(READLINE_DIR)/readline
	$(CC) $(MLIBC_APP_CFLAGS) -I$(READLINE_DIR) -c $< -o $@

$(APPS_BUILD_DIR)/readlinetest/readlinetest: $(APPS_BUILD_DIR)/readlinetest/readlinetest.c.o $(READLINE_BUILD_DIR)/libreadline.a apps/link/readlinetest.ld
	ld.lld -nostdlib -static -T apps/link/readlinetest.ld -e _start -o $@ \
	    $(MLIBC_CRT_OBJS) $(APPS_BUILD_DIR)/readlinetest/readlinetest.c.o $(READLINE_BUILD_DIR)/libreadline.a $(MLIBC_LIBS)
	$(STRIP) --strip-all $@

readlinetest: $(APPS_BUILD_DIR)/readlinetest/readlinetest

$(APPS_BUILD_DIR)/stub/stub.c.o: apps/stub/stub.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c $< -o $@

$(APPS_BUILD_DIR)/stub/stub: $(APPS_BUILD_DIR)/stub/stub.c.o apps/link/stub.ld
	$(LD) $(APP_LDFLAGS) -T apps/link/stub.ld -e _start -o $@ \
	    $(APPS_BUILD_DIR)/stub/stub.c.o

ROOTFS_STUB_NAMES := root_task file
ROOTFS_BUSYBOX_SYMLINKS := awk base64 basename blkid cat cksum clear cmp cp cut \
                          date dd dirname du echo env expand factor false find fold \
                          free grep head hexdump hostname kill link ls md5sum mkdir \
                          mknod mv nohup od paste pgrep pidof pkill printf ps pstree \
                          pwd rm rmdir sed seq sha1sum sha256sum sha512sum sleep sort \
                          stat sync tail tee test touch tr true tty unexpand uniq unlink \
                          wc whoami xargs yes

$(ROOTFS_STAGE_STAMP): Makefile $(APPS_BUILD_DIR)/devfs/devfs $(APPS_BUILD_DIR)/console_driver/console_driver \
                         $(APPS_BUILD_DIR)/ramfs/ramfs \
                         $(APPS_BUILD_DIR)/procfs/procfs $(APPS_BUILD_DIR)/sysfs/sysfs \
                         $(APPS_BUILD_DIR)/diskfs/diskfs \
                         $(APPS_BUILD_DIR)/pager/pager \
                         $(APPS_BUILD_DIR)/powertools/reboot \
                         $(APPS_BUILD_DIR)/powertools/halt \
                         $(APPS_BUILD_DIR)/powertools/shutdown \
                         $(APPS_BUILD_DIR)/sigtest/sigtest \
                         $(APPS_BUILD_DIR)/consoletest/consoletest \
                         $(APPS_BUILD_DIR)/mousetest/mousetest \
                         $(APPS_BUILD_DIR)/fbtest/fbtest \
                         $(APPS_BUILD_DIR)/pcitest/pcitest \
                         $(APPS_BUILD_DIR)/diskfstest_write/diskfstest_write \
                         $(APPS_BUILD_DIR)/diskfstest_verify/diskfstest_verify \
                         $(APPS_BUILD_DIR)/blockdrv/blockdrv \
                         $(APPS_BUILD_DIR)/fat16fs/fat16fs \
                         $(APPS_BUILD_DIR)/fat16fstest/fat16fstest \
                         $(APPS_BUILD_DIR)/fat16fstest_write/fat16fstest_write \
                         $(APPS_BUILD_DIR)/fat32fs/fat32fs \
                         $(APPS_BUILD_DIR)/fat32fstest/fat32fstest \
                         $(APPS_BUILD_DIR)/ext2fs/ext2fs \
                         $(APPS_BUILD_DIR)/ext2fstest/ext2fstest \
                         $(APPS_BUILD_DIR)/ext2fsroot/ext2fsroot \
                         $(APPS_BUILD_DIR)/shmtest_producer/shmtest_producer \
                         $(APPS_BUILD_DIR)/shmtest_consumer/shmtest_consumer \
                         $(APPS_BUILD_DIR)/socktest_server/socktest_server \
                         $(APPS_BUILD_DIR)/socktest_client/socktest_client \
                         $(APPS_BUILD_DIR)/mlibc-hello/hello \
                         $(APPS_BUILD_DIR)/linuxvfstest/linuxvfstest \
                         $(APPS_BUILD_DIR)/am/am \
                         $(APPS_BUILD_DIR)/top/top \
                         $(APPS_BUILD_DIR)/readlinetest/readlinetest \
                         $(APPS_BUILD_DIR)/vfs_mount/mount_devfs \
                         $(APPS_BUILD_DIR)/vfs_mount/mount_procfs \
                         $(APPS_BUILD_DIR)/vfs_mount/mount_sysfs \
                         $(APPS_BUILD_DIR)/vfs_mount/mount_tmpfs \
                         $(APPS_BUILD_DIR)/hello_initsys/hello_initsys \
                         $(APPS_BUILD_DIR)/tty_service/tty_service \
                         $(BUSYBOX_BIN) \
                         $(APPS_BUILD_DIR)/stub/stub \
                         $(BASH_DIR)/bash \
                         apps/hello_initsys/rc.conf etc/passwd
	rm -rf $(ROOTFS_STAGE)
	mkdir -p $(ROOTFS_STAGE) $(ROOTFS_STAGE)/bin $(ROOTFS_STAGE)/sbin \
	         $(ROOTFS_STAGE)/etc $(ROOTFS_STAGE)/usr/bin $(ROOTFS_STAGE)/usr/sbin \
	         $(ROOTFS_STAGE)/Core/Servers
	cp $(APPS_BUILD_DIR)/devfs/devfs $(ROOTFS_STAGE)/devfs
	cp $(APPS_BUILD_DIR)/console_driver/console_driver $(ROOTFS_STAGE)/console_driver
	cp $(APPS_BUILD_DIR)/ramfs/ramfs $(ROOTFS_STAGE)/ramfs
	cp $(APPS_BUILD_DIR)/procfs/procfs $(ROOTFS_STAGE)/procfs
	cp $(APPS_BUILD_DIR)/sysfs/sysfs $(ROOTFS_STAGE)/sysfs
	cp $(APPS_BUILD_DIR)/diskfs/diskfs $(ROOTFS_STAGE)/diskfs
	cp $(APPS_BUILD_DIR)/pager/pager $(ROOTFS_STAGE)/pager
	cp $(APPS_BUILD_DIR)/sigtest/sigtest $(ROOTFS_STAGE)/sigtest
	cp $(APPS_BUILD_DIR)/consoletest/consoletest $(ROOTFS_STAGE)/consoletest
	cp $(APPS_BUILD_DIR)/mousetest/mousetest $(ROOTFS_STAGE)/mousetest
	cp $(APPS_BUILD_DIR)/fbtest/fbtest $(ROOTFS_STAGE)/fbtest
	cp $(APPS_BUILD_DIR)/pcitest/pcitest $(ROOTFS_STAGE)/pcitest
	cp $(APPS_BUILD_DIR)/diskfstest_write/diskfstest_write $(ROOTFS_STAGE)/diskfstest_write
	cp $(APPS_BUILD_DIR)/diskfstest_verify/diskfstest_verify $(ROOTFS_STAGE)/diskfstest_verify
	cp $(APPS_BUILD_DIR)/blockdrv/blockdrv $(ROOTFS_STAGE)/blockdrv
	cp $(APPS_BUILD_DIR)/fat16fs/fat16fs $(ROOTFS_STAGE)/fat16fs
	cp $(APPS_BUILD_DIR)/fat16fstest/fat16fstest $(ROOTFS_STAGE)/fat16fstest
	cp $(APPS_BUILD_DIR)/fat16fstest_write/fat16fstest_write $(ROOTFS_STAGE)/fat16fstest_write
	cp $(APPS_BUILD_DIR)/fat32fs/fat32fs $(ROOTFS_STAGE)/fat32fs
	cp $(APPS_BUILD_DIR)/fat32fstest/fat32fstest $(ROOTFS_STAGE)/fat32fstest
	cp $(APPS_BUILD_DIR)/ext2fs/ext2fs $(ROOTFS_STAGE)/ext2fs
	cp $(APPS_BUILD_DIR)/ext2fs/ext2fs $(ROOTFS_STAGE)/bin/ext2fs
	cp $(APPS_BUILD_DIR)/ext2fstest/ext2fstest $(ROOTFS_STAGE)/ext2fstest
	cp $(APPS_BUILD_DIR)/ext2fstest/ext2fstest $(ROOTFS_STAGE)/bin/ext2fstest
	cp $(APPS_BUILD_DIR)/ext2fsroot/ext2fsroot $(ROOTFS_STAGE)/ext2fsroot
	cp $(APPS_BUILD_DIR)/shmtest_producer/shmtest_producer $(ROOTFS_STAGE)/shmtest_producer
	cp $(APPS_BUILD_DIR)/shmtest_consumer/shmtest_consumer $(ROOTFS_STAGE)/shmtest_consumer
	cp $(APPS_BUILD_DIR)/socktest_server/socktest_server $(ROOTFS_STAGE)/socktest_server
	cp $(APPS_BUILD_DIR)/socktest_client/socktest_client $(ROOTFS_STAGE)/socktest_client
	cp $(APPS_BUILD_DIR)/mlibc-hello/hello $(ROOTFS_STAGE)/mlibc-hello

	cp $(APPS_BUILD_DIR)/linuxvfstest/linuxvfstest $(ROOTFS_STAGE)/bin/linuxvfstest
	cp $(APPS_BUILD_DIR)/readlinetest/readlinetest $(ROOTFS_STAGE)/readlinetest
	cp $(APPS_BUILD_DIR)/powertools/reboot $(ROOTFS_STAGE)/sbin/reboot
	cp $(APPS_BUILD_DIR)/powertools/halt $(ROOTFS_STAGE)/sbin/halt
	cp $(APPS_BUILD_DIR)/powertools/shutdown $(ROOTFS_STAGE)/sbin/shutdown
	cp $(APPS_BUILD_DIR)/vfs_mount/mount_devfs $(ROOTFS_STAGE)/Core/Servers/mount_devfs
	cp $(APPS_BUILD_DIR)/vfs_mount/mount_procfs $(ROOTFS_STAGE)/Core/Servers/mount_procfs
	cp $(APPS_BUILD_DIR)/vfs_mount/mount_sysfs $(ROOTFS_STAGE)/Core/Servers/mount_sysfs
	cp $(APPS_BUILD_DIR)/vfs_mount/mount_tmpfs $(ROOTFS_STAGE)/Core/Servers/mount_tmpfs
	cp $(APPS_BUILD_DIR)/am/am $(ROOTFS_STAGE)/bin/am
	cp $(APPS_BUILD_DIR)/top/top $(ROOTFS_STAGE)/bin/top
	cp $(APPS_BUILD_DIR)/hello_initsys/hello_initsys $(ROOTFS_STAGE)/usr/sbin/hello_initsys
	cp $(APPS_BUILD_DIR)/tty_service/tty_service $(ROOTFS_STAGE)/Core/Servers/tty_service
	cp apps/hello_initsys/rc.conf $(ROOTFS_STAGE)/etc/rc.conf
	cp etc/passwd $(ROOTFS_STAGE)/etc/passwd
	cp $(BUSYBOX_BIN) $(ROOTFS_STAGE)/bin/busybox
	cp $(BASH_DIR)/bash $(ROOTFS_STAGE)/bin/bash
	ln -sf bin/bash $(ROOTFS_STAGE)/bin/sh
	cp $(APPS_BUILD_DIR)/stub/stub $(ROOTFS_STAGE)/bin/file
	for n in $(ROOTFS_STUB_NAMES); do cp $(APPS_BUILD_DIR)/stub/stub $(ROOTFS_STAGE)/$$n; done
	for n in $(ROOTFS_BUSYBOX_SYMLINKS); do ln -sf busybox $(ROOTFS_STAGE)/bin/$$n; done
	touch $@

QEMU ?= qemu-system-x86_64
QEMU_SMP ?= 2
QEMU_MEM ?= 256
QEMU_APPEND ?= root=root_task starter=hello_initsys

QEMU_DISK ?= $(BUILD_DIR)/diskfs.img

run: $(KERNEL_DISK_IMG)
	@test -f $(QEMU_DISK) || truncate -s 16M $(QEMU_DISK)
	$(QEMU) -drive file=$(KERNEL_DISK_IMG),format=raw -boot c \
	    -smp $(QEMU_SMP) -m $(QEMU_MEM) \
	    -nographic -device isa-debug-exit,iobase=0xf4,iosize=0x04 \
	    -drive file=$(QEMU_DISK),format=raw,if=none,id=blk0 \
	    -device virtio-blk-pci,drive=blk0

run-gui: $(KERNEL_DISK_IMG)
	QEMU_DISK=$(QEMU_DISK) ./scripts/run-gui.sh $(KERNEL_DISK_IMG)

# --- TUI AM BUILD RULES (NATIVE FREESTANDING) ---
$(APPS_BUILD_DIR)/am/am.c.o: apps/am/am.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c $< -o $@

$(APPS_BUILD_DIR)/am/am: $(APPS_BUILD_DIR)/am/am.c.o apps/link/am.ld
	$(LD) $(APP_LDFLAGS) -T apps/link/am.ld -e _start -o $@ \
	    $(APPS_BUILD_DIR)/am/am.c.o
	$(STRIP) --strip-all $@

# --- TUI TOP BUILD RULES (NATIVE FREESTANDING) ---
$(APPS_BUILD_DIR)/top/top.c.o: apps/top/top.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c $< -o $@

$(APPS_BUILD_DIR)/top/top: $(APPS_BUILD_DIR)/top/top.c.o apps/link/top.ld
	$(LD) $(APP_LDFLAGS) -T apps/link/top.ld -e _start -o $@ \
	    $(APPS_BUILD_DIR)/top/top.c.o
	$(STRIP) --strip-all $@
