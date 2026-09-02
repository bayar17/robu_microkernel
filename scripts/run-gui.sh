#!/bin/bash
set -e
cd "$(dirname "$0")/.."

QEMU="${QEMU:-qemu-system-x86_64}"
QEMU_SMP="${QEMU_SMP:-2}"
QEMU_MEM="${QEMU_MEM:-256}"
QEMU_DISK="${QEMU_DISK:-build/diskfs.img}"
KERNEL_DISK_IMG="${1:-build/robu-kernel-disk.img}"

test -f "$QEMU_DISK" || truncate -s 16M "$QEMU_DISK"

case "$(uname -s)" in
    Darwin)
        DISPLAY_BACKEND=cocoa
        ;;
    Linux)
        if "$QEMU" -display help 2>/dev/null | grep -q '^gtk$'; then
            DISPLAY_BACKEND=gtk
        elif "$QEMU" -display help 2>/dev/null | grep -q '^sdl$'; then
            DISPLAY_BACKEND=sdl
        else
            echo "run-gui.sh: no gtk or sdl display backend available in $QEMU" >&2
            exit 1
        fi
        ;;
    *)
        echo "run-gui.sh: unsupported host OS $(uname -s)" >&2
        exit 1
        ;;
esac

exec "$QEMU" -drive file="$KERNEL_DISK_IMG",format=raw -boot c \
    -smp "$QEMU_SMP" -m "$QEMU_MEM" \
    -display "$DISPLAY_BACKEND" \
    -device isa-debug-exit,iobase=0xf4,iosize=0x04 \
    -drive file="$QEMU_DISK",format=raw,if=none,id=blk0 \
    -device virtio-blk-pci,drive=blk0
