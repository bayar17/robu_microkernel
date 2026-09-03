# Robu 0.9

Release date: 2026-09-03

Robu 0.9 is an alpha release of the x86_64 microkernel and its freestanding userland. The kernel identifies itself as `Robu Kernel 0.9 x86_64` and exposes kernel ABI version 1.2.

## Highlights

- Robu now boots without GRUB. The repository builds its own legacy BIOS stage1 and stage2 loader, including ext2 and FAT32 read support.
- The hybrid disk image supports both BIOS and UEFI boot. Its ESP contains the standard removable-media loader at `EFI/BOOT/BOOTX64.EFI`.
- The root filesystem is a real ext2 filesystem populated at build time. `rootfs.tar` is retired from the kernel execution path and is not placed in the root image.
- Bootstrap servers are loaded directly from `/boot/bootstrap/`; normal executables are read from the ext2 filesystem by the ring-3 ext2 server at execution time.
- Bash, Readline, mlibc, and BusyBox provide an interactive static userland. BusyBox applets are installed through normal command names such as `ls`, `cat`, `clear`, and `blkid`.
- The boot image exposes its actual `/boot` contents: `kernel.elf`, `cmdline.txt`, and bootstrap modules.
- The ring-3 filesystem and device-server set includes ext2, FAT16, FAT32, ramfs, devfs, procfs, sysfs, blockdrv, and diskfs.
- The kernel ABI adds random, Robu VFS, and Linux VFS feature reporting.
- The filesystem servers are organized under `src/servers/fs/`, with separate ext2, FAT16, FAT32, and ramfs components.
- The userland image places `tty_service` and the `mount_*` helpers under `/Core/Servers`.
- The repository includes xHCI support, framebuffer console improvements, Linux-style block-device exposure through devfs, and an ext2 mutation and reboot test matrix checked by host `e2fsck`.

## Boot images

Build the normal BIOS disk:

```sh
make all
make bootloader-kernel-disk
```

This creates `build/robu-kernel-disk.img`.

Build the recommended GPT image for UTM or physical media:

```sh
make all
make bootloader-hybrid-disk
```

This creates `build/robu-hybrid-disk.img` with an ext2 `robu-root` partition and a FAT32 EFI System Partition.

## Validation

This release was boot-verified with the custom BIOS loader and with the UEFI loader under QEMU and OVMF. The tested path reaches the bootstrap servers, runs `hello_initsys`, mounts the runtime filesystems, starts the terminal services, and reaches an interactive Bash prompt.

The ext2 hardening matrix is available with:

```sh
make ext2-hardening-test
```

The diskfs persistence check is available with:

```sh
./scripts/diskfs-persist-test.sh
```

## Upgrade notes

- This is a raw disk-image release, not an ISO release.
- UEFI removable-media boot uses `EFI/BOOT/BOOTX64.EFI`. A custom NVRAM boot entry is not required.
- Attach a separate writable virtio-blk disk for `diskfs`, such as `build/diskfs.img`. Do not use the boot disk for diskfs: diskfs owns LBA 0 on its backing disk and will corrupt boot sectors if both roles share one image.
- The root filesystem is now populated during image construction. There is no runtime rootfs tar extraction and no `/boot/rootfs.tar`.
- Minibox is replaced by BusyBox. Existing scripts should invoke BusyBox applets by their normal names, not as `busybox <applet>`.
- The checked-in submodule URLs now use GitHub, so a fresh checkout can use `git clone --recurse-submodules`.

## Known limitations

- Robu remains alpha software. Every hardware configuration needs a real boot test.
- The system is fully static. Dynamic linking and runtime module loading are not available.
- Ext2 metadata uses Robu's redo journal, stored in a reserved ext2 file. It preserves ext2 compatibility but is not Linux ext3/JBD.
- File data is ordered before its metadata commit, but the journal has a 64-sector metadata transaction limit. Oversized metadata changes fail safely rather than writing a partial transaction.
- The ext2 test matrix is stronger than basic read and write testing, but it does not replace power-loss testing on physical storage.
- Hardware support, especially USB devices and graphics timing, is still under active development and varies by machine.

## Source revision

The implementation checkpoint for this release is recorded in the repository Git history, including the matching Robu mlibc sysdeps revision.
