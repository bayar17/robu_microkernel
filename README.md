<div align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="images/logo-robu-dark.svg">
    <img src="images/logo-robu.svg" alt="Robu Microkernel" width="320">
  </picture>
</div>

<div align="center">

![Target](https://img.shields.io/badge/Target-x86__64-red)
![Boot](https://img.shields.io/badge/Boot-BIOS%20%7C%20UEFI-427819)
![Build](https://img.shields.io/badge/Build-Make%20%2B%20Docker-2496ED)
![License](https://img.shields.io/badge/License-MIT-blue)
![Status](https://img.shields.io/badge/Status-alpha-orange)
![GitHub Actions Workflow Status](https://img.shields.io/github/actions/workflow/status/bayar17/robu_microkernel/ci.yml)

</div>

## Robu Microkernel

Robu is a real x86_64 microkernel written from scratch in C and assembly. It has a small kernel surface for threads, address spaces, IPC, virtual memory, scheduling, capabilities, and hardware permissions. Drivers, filesystems, and most operating-system policy run as ring-3 servers.

The repository builds a static, freestanding system with no dynamic linker and no GRUB. Its boot images use Robu's own loaders:

- Legacy BIOS stage1 and stage2 code with ext2 and FAT32 readers.
- A UEFI application at the standard removable-media path `EFI/BOOT/BOOTX64.EFI`.
- A tarless ext2 root filesystem. `/boot` contains `kernel.elf`, `cmdline.txt`, and direct bootstrap modules, not `rootfs.tar`.
- Bash, Readline, mlibc, and BusyBox. BusyBox utilities are available through normal names such as `ls`, `cat`, `clear`, and `blkid`.
- Ring-3 filesystem and device servers, including ext2, FAT16, FAT32, ramfs, devfs, procfs, sysfs, blockdrv, and diskfs.

Robu is alpha software. A successful build does not substitute for a real QEMU or hardware boot test.

Current release: [Robu 0.9 release notes](RELEASE_NOTES.md).

## Requirements

The kernel and userland build run inside Docker or Podman. The bootloader and disk-image steps run on the host, so install the host tools too.

| Purpose | Required tools |
| --- | --- |
| Source checkout | Git and access to Robu's submodule remotes, or the public-remotes setup below |
| Container build | Docker Desktop with a running daemon, or Podman with a running machine |
| Bootloader build | Clang, LLD, LLVM tools including `llvm-objcopy` and `llvm-ar`, GNU Make, Python 3 |
| ext2 root image | `mke2fs`, `debugfs`, and `e2fsck` from e2fsprogs |
| FAT32 ESP | mtools: `mformat`, `mmd`, and `mcopy` |
| Hybrid GPT image | `sgdisk` from gptfdisk or gdisk |
| QEMU testing | `qemu-system-x86_64` |
| UEFI QEMU testing | OVMF code and variable-store firmware files |
| Optional ext2 hardening matrix | `genext2fs` |

The container image supplies Clang, LLD, LLVM, Meson, Flex, Bison, mtools, e2fsprogs, QEMU, and the libraries needed by mlibc and the userland build. `make all` builds that image automatically.

### macOS

Install the host tools with Homebrew:

```sh
brew install llvm bison flex e2fsprogs mtools qemu gptfdisk meson python genext2fs
brew install --cask docker
```

Start Docker Desktop before building. Podman is also supported if its machine is running.

Homebrew does not always put LLVM and e2fsprogs on `PATH`. Add them for the current shell before building an image:

```sh
export PATH="$(brew --prefix llvm)/bin:$(brew --prefix bison)/bin:$(brew --prefix e2fsprogs)/sbin:$PATH"
```

On Apple Silicon, the Makefile defaults for OVMF normally resolve under `/opt/homebrew/share/qemu`. On another Homebrew prefix, locate the two firmware files and pass their paths to the EFI target:

```sh
make run-hybrid-disk-efi \
  OVMF_CODE="/path/to/edk2-x86_64-code.fd" \
  OVMF_VARS_TEMPLATE="/path/to/edk2-i386-vars.fd"
```

### Debian and Ubuntu

Install the host build and test dependencies with:

```sh
sudo apt-get update
sudo apt-get install -y \
  clang lld llvm make flex bison mtools e2fsprogs gdisk \
  qemu-system-x86 ovmf meson libc++-dev libc++abi-dev \
  python3 git patch bzip2 genext2fs
```

Install and start Docker, or install Podman and start its machine, before running `make all`.

## Clone

Clone the repository and every pinned GitHub submodule:

```sh
git clone --recurse-submodules https://github.com/bayar17/robu_microkernel.git
cd robu_microkernel
```

The tree uses Robu-maintained GitHub forks for mlibc, libconfuse, Readline, and Bash; the upstream uACPI repository; and BusyBox's official GitHub mirror. The pinned uACPI revision is its `6.0.0` tag.

## Build a bootable system

Run the kernel and userland build first:

```sh
make all
```

`make all` routes the kernel and userland build through the available container engine. Ensure the Docker daemon or Podman service you intend to use is running. The kernel Makefile intentionally rejects direct non-Linux host compilation, so on macOS a working Docker or Podman engine is required.

Then choose an image format.

### BIOS disk image

Build the standard raw disk image for legacy BIOS:

```sh
make bootloader-kernel-disk
```

Output:

```text
build/robu-kernel-disk.img
```

This image has Robu's BIOS boot sector and an ext2 root filesystem at LBA 2048. It is not an ISO.

### Hybrid BIOS and UEFI disk image

Build the recommended image for UTM or real hardware:

```sh
make bootloader-hybrid-disk
```

Output:

```text
build/robu-hybrid-disk.img
```

The hybrid image has a GPT with two partitions:

| Partition | Contents |
| --- | --- |
| `robu-root` | ext2 root filesystem containing `/boot/kernel.elf`, `/boot/cmdline.txt`, and `/boot/bootstrap/` |
| `EFI System` | FAT32 ESP containing `EFI/BOOT/BOOTX64.EFI` |

The same raw image is bootable through the BIOS loader or the UEFI loader. `BOOTX64.EFI` is the correct UEFI fallback filename for removable media. No UEFI NVRAM entry and no file named `ROBUSTIC.EFI` are required.

## Run in QEMU

The quickest serial-console boot is:

```sh
make all
make run
```

`make run` creates `build/diskfs.img` when needed and attaches it as a separate virtio-blk disk. Keep that separate disk: `diskfs` writes its own metadata at LBA 0, so using the boot image for both roles will corrupt the bootloader after a successful boot.

For a graphical QEMU window:

```sh
make run-gui
```

For the hybrid image specifically:

```sh
make run-hybrid-disk-bios
make run-hybrid-disk-efi
```

The EFI target requires OVMF. Use the `OVMF_CODE` and `OVMF_VARS_TEMPLATE` overrides shown in the macOS section when the firmware is not at the Makefile default location.

The usual QEMU settings can be overridden on the command line:

```sh
make run QEMU_SMP=4 QEMU_MEM=512
```

`QEMU_APPEND` is baked into `/boot/cmdline.txt` while the ext2 image is assembled. To change it, make a clean image build:

```sh
make clean
make all
make bootloader-kernel-disk \
  QEMU_APPEND='root=root_task starter=hello_initsys shmtest=1'
make run
```

## Use in UTM

1. Build `build/robu-hybrid-disk.img` with `make all` followed by `make bootloader-hybrid-disk`.
2. Create an x86_64 QEMU virtual machine in UTM and attach `build/robu-hybrid-disk.img` as its first raw disk.
3. Create a separate 16 MiB raw disk with `truncate -s 16M build/diskfs.img` and attach it as a second VirtIO disk.
4. Use UEFI firmware to exercise `EFI/BOOT/BOOTX64.EFI`, or legacy BIOS firmware to exercise stage1 and stage2.
5. Allocate at least 512 MiB of memory and two virtual CPUs for a representative boot.

The second disk is required for writable diskfs state and must not replace the boot disk.

## Userland layout

The final ext2 root is populated at build time. It does not unpack a tarball at runtime.

| Location | Contents |
| --- | --- |
| `/boot` | `kernel.elf`, `cmdline.txt`, and direct bootstrap modules |
| `/bin` | Bash, BusyBox, BusyBox applet symlinks, and normal user commands |
| `/sbin` | Power-control utilities |
| `/Core/Servers` | `mount_devfs`, `mount_procfs`, `mount_sysfs`, `mount_tmpfs`, and `tty_service` |
| `/etc` | `rc.conf`, `passwd`, `group`, `shells`, and `bashrc` |

BusyBox is installed as `/bin/busybox` and the enabled applets are symbolic links to it. Type `ls`, `clear`, `blkid`, `cat`, or another enabled applet directly; do not prefix each command with `busybox`.

## Useful targets

| Command | Result |
| --- | --- |
| `make all` | Docker or Podman build of the kernel and required userland |
| `make mlibc` | Build and install Robu's mlibc sysroot |
| `make busybox` | Build the static BusyBox binary |
| `make readline` | Build static Readline |
| `make bash-configure` | Configure Bash against the Robu mlibc and Readline build |
| `make bash-build` | Build Bash |
| `make bootloader` | Build the small BIOS stage1/stage2 diagnostic image |
| `make bootloader-kernel-disk` | Build the regular BIOS boot disk |
| `make bootloader-hybrid-disk` | Build the GPT image with BIOS and UEFI boot support |
| `make run` | Boot the BIOS disk in serial QEMU with a separate diskfs disk |
| `make run-gui` | Boot the BIOS disk in a graphical QEMU window |
| `make run-hybrid-disk-bios` | Boot the hybrid image through BIOS QEMU |
| `make run-hybrid-disk-efi` | Boot the hybrid image through OVMF QEMU |
| `make ext2-hardening-test` | Run the ext2 mutation, reboot, and host `e2fsck` matrix |
| `scripts/diskfs-persist-test.sh` | Verify diskfs data survives two clean QEMU boots |
| `make clean` | Remove the complete `build/` directory |

## Validation

Use a real boot log as the source of truth. A successful compilation alone does not prove the image boots or the root filesystem is usable.

The ext2 hardening target builds test images, runs guest mutations, reboots them, and checks the resulting filesystems with host `e2fsck`:

```sh
make ext2-hardening-test
```

It needs `genext2fs`, e2fsprogs, QEMU, and OVMF. It takes several minutes and writes temporary images under `/tmp`.

The diskfs persistence test starts two QEMU boots and checks a file created during the first boot is visible during the second:

```sh
./scripts/diskfs-persist-test.sh
```

## Troubleshooting

### Docker or Podman is unavailable

Start Docker Desktop, or start the Podman machine, then confirm one of these succeeds:

```sh
docker info
podman info
```

On macOS, `make all` cannot fall back to a direct kernel build.

### Meson reports stale or incompatible build data

Discard the generated mlibc build directory and build again:

```sh
rm -rf build/mlibc build/mlibc-sysroot
make all
```

### `llvm-objcopy`, `ld.lld`, or `lld-link` is not found

Install LLVM and put its `bin` directory on `PATH`. The macOS command in the requirements section does that without assuming an Intel or Apple Silicon Homebrew prefix.

### `mke2fs`, `debugfs`, or `e2fsck` is not found on macOS

Install e2fsprogs and add its `sbin` directory to `PATH`:

```sh
brew install e2fsprogs
export PATH="$(brew --prefix e2fsprogs)/sbin:$PATH"
```

### The next boot hangs after a previous successful boot

Check that the virtual machine has a separate `diskfs.img` virtio-blk disk. If diskfs uses the boot disk, it overwrites the boot sectors by design.

## License

MIT. See [LICENSE](LICENSE).
