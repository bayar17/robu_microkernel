#!/bin/sh

set -eu

cd "$(dirname "$0")/.."

GENEXT2FS="${GENEXT2FS:-$(command -v genext2fs 2>/dev/null || true)}"
E2FSCK="${E2FSCK:-$(command -v e2fsck 2>/dev/null || true)}"
if [ -z "$E2FSCK" ] && [ -x /opt/homebrew/opt/e2fsprogs/sbin/e2fsck ]; then
    E2FSCK=/opt/homebrew/opt/e2fsprogs/sbin/e2fsck
fi
MKE2FS="${MKE2FS:-$(command -v mke2fs 2>/dev/null || true)}"
if [ -z "$MKE2FS" ] && [ -x /opt/homebrew/opt/e2fsprogs/sbin/mke2fs ]; then
    MKE2FS=/opt/homebrew/opt/e2fsprogs/sbin/mke2fs
fi
DEBUGFS="${DEBUGFS:-$(command -v debugfs 2>/dev/null || true)}"
if [ -z "$DEBUGFS" ] && [ -x /opt/homebrew/opt/e2fsprogs/sbin/debugfs ]; then
    DEBUGFS=/opt/homebrew/opt/e2fsprogs/sbin/debugfs
fi
QEMU="${QEMU:-$(command -v qemu-system-x86_64 2>/dev/null || true)}"
OVMF_CODE="${OVMF_CODE:-/opt/homebrew/share/qemu/edk2-x86_64-code.fd}"
OVMF_VARS_TEMPLATE="${OVMF_VARS_TEMPLATE:-/opt/homebrew/share/qemu/edk2-i386-vars.fd}"
EXT2_BLOCK_SIZES="${EXT2_BLOCK_SIZES:-1024 2048 4096}"
TEST_EXIT_DELAY="${EXT2_TEST_EXIT_DELAY:-90}"
REBUILD_IMAGE="${EXT2_HARDENING_REBUILD_IMAGE:-1}"

for tool in "$GENEXT2FS" "$E2FSCK" "$MKE2FS" "$DEBUGFS" "$QEMU"; do
    if [ -z "$tool" ] || [ ! -x "$tool" ]; then
        echo "FAIL: missing required host tool: $tool" >&2
        exit 1
    fi
done

if [ ! -f "$OVMF_CODE" ] || [ ! -f "$OVMF_VARS_TEMPLATE" ]; then
    echo "FAIL: OVMF firmware files not found" >&2
    exit 1
fi

WORK_DIR=$(mktemp -d /tmp/robu-ext2-hardening-XXXXXX)
KEEP_ARTIFACTS="${KEEP_EXT2_HARDENING_ARTIFACTS:-0}"
DISKFS_IMAGE="$WORK_DIR/diskfs.img"
truncate -s 16M "$DISKFS_IMAGE"

cleanup() {
    status=$?
    if [ "$status" -eq 0 ] && [ "$KEEP_ARTIFACTS" = 0 ]; then
        rm -rf "$WORK_DIR"
    else
        echo "artifacts preserved at $WORK_DIR" >&2
    fi
    exit "$status"
}

trap cleanup EXIT

fixture_root="$WORK_DIR/fixture-root"
mkdir -p "$fixture_root/bin"
printf 'Hello from a real ext2 filesystem, verified via e2fsprogs.\n' > "$fixture_root/testfile.txt"
printf 'seed\n' > "$fixture_root/bin/seed.txt"

if [ "$REBUILD_IMAGE" = 1 ]; then
    make all
    make -B bootloader-hybrid-disk
else
    make build/apps/ext2fs/ext2fs build/apps/ext2fstest/ext2fstest
fi

BASE_DISK=build/robu-hybrid-disk.img
BASE_ROOTFS=build/bootloader/ext2-kernel.img
EXT2_SERVER_BIN=build/apps/ext2fs/ext2fs
EXT2_TEST_BIN=build/apps/ext2fstest/ext2fstest
if [ ! -f "$BASE_DISK" ] || [ ! -f "$BASE_ROOTFS" ]; then
    echo "FAIL: hybrid boot image build did not produce the expected ext2 root" >&2
    exit 1
fi
if [ ! -f "$EXT2_SERVER_BIN" ] || [ ! -f "$EXT2_TEST_BIN" ]; then
    echo "FAIL: ext2 guest binaries are missing" >&2
    exit 1
fi

make_boot_disk() {
    disk=$1
    cmdline=$2
    rootfs="$WORK_DIR/rootfs.img"
    debugfs_script="$WORK_DIR/cmdline.debugfs"
    cmdline_file="$WORK_DIR/cmdline.txt"
    cp "$BASE_DISK" "$disk"
    cp "$BASE_ROOTFS" "$rootfs"
    printf '%s' "$cmdline" > "$cmdline_file"
    printf 'cd /\nrm /boot/cmdline.txt\nwrite %s /boot/cmdline.txt\nrm /bin/ext2fs\nwrite %s /bin/ext2fs\nrm /bin/ext2fstest\nwrite %s /bin/ext2fstest\n' \
        "$cmdline_file" "$(pwd)/$EXT2_SERVER_BIN" "$(pwd)/$EXT2_TEST_BIN" > "$debugfs_script"
    "$DEBUGFS" -w -f "$debugfs_script" "$rootfs" >/dev/null
    dd if="$rootfs" of="$disk" bs=512 seek=2048 conv=notrunc status=none
}

check_fs() {
    image=$1
    phase=$2
    echo "=== e2fsck: $phase ==="
    "$E2FSCK" -fn "$image"
}

check_guest_result() {
    log=$1
    if ! awk '
        /\[ext2fs-test\]/ {
            sub(/^.*\[ext2fs-test\][[:space:]]+/, "");
            split($1, fields, "/");
            if (fields[1] == fields[2] && fields[1] != "0") ok = 1;
        }
        END { exit ok ? 0 : 1 }
    ' "$log"; then
        echo "FAIL: the guest ext2 test did not report all checks passing" >&2
        cat "$log" >&2
        exit 1
    fi
}

boot_guest() {
    disk=$1
    test_image=$2
    log=$3
    vars="$WORK_DIR/ovmf-vars.fd"
    cp "$OVMF_VARS_TEMPLATE" "$vars"
    set +e
    "$QEMU" \
        -drive if=pflash,format=raw,readonly=on,file="$OVMF_CODE" \
        -drive if=pflash,format=raw,file="$vars" \
        -device qemu-xhci,id=xhci \
        -drive file="$disk",format=raw,if=none,id=usbroot \
        -device usb-storage,drive=usbroot \
        -drive file="$test_image",format=raw,if=ide,index=0 \
        -drive file="$DISKFS_IMAGE",format=raw,if=none,id=blk0 \
        -device virtio-blk-pci,drive=blk0 \
        -smp 2 -m 512 -display none -serial file:"$log" -monitor none \
        -no-reboot -device isa-debug-exit,iobase=0xf4,iosize=0x04
    qemu_status=$?
    set -e
    if [ "$qemu_status" -eq 0 ]; then
        echo "FAIL: QEMU exited before the guest test exit" >&2
        cat "$log" >&2
        exit 1
    fi
    check_guest_result "$log"
}

boot_rejected_image() {
    disk=$1
    test_image=$2
    log=$3
    vars="$WORK_DIR/ovmf-vars.fd"
    cp "$OVMF_VARS_TEMPLATE" "$vars"
    set +e
    "$QEMU" \
        -drive if=pflash,format=raw,readonly=on,file="$OVMF_CODE" \
        -drive if=pflash,format=raw,file="$vars" \
        -device qemu-xhci,id=xhci \
        -drive file="$disk",format=raw,if=none,id=usbroot \
        -device usb-storage,drive=usbroot \
        -drive file="$test_image",format=raw,if=ide,index=0 \
        -drive file="$DISKFS_IMAGE",format=raw,if=none,id=blk0 \
        -device virtio-blk-pci,drive=blk0 \
        -smp 2 -m 512 -display none -serial file:"$log" -monitor none \
        -no-reboot -device isa-debug-exit,iobase=0xf4,iosize=0x04
    qemu_status=$?
    set -e
    if [ "$qemu_status" -eq 0 ]; then
        echo "FAIL: QEMU exited before the feature-rejection watchdog" >&2
        cat "$log" >&2
        exit 1
    fi
    if ! grep -F "[ext2fs-reject] mount status=1" "$log" >/dev/null; then
        echo "FAIL: ext2fs did not reject the unsupported filesystem image" >&2
        cat "$log" >&2
        exit 1
    fi
}

for block_size in $EXT2_BLOCK_SIZES; do
    case "$block_size" in
        1024) blocks=131072 ;;
        2048) blocks=65536 ;;
        4096) blocks=32768 ;;
        *)
            echo "FAIL: unsupported block size $block_size" >&2
            exit 1
            ;;
    esac

    test_image="$WORK_DIR/ext2-${block_size}.img"
    mutation_disk="$WORK_DIR/boot-mutation-${block_size}.img"
    reboot_disk="$WORK_DIR/boot-reboot-${block_size}.img"
    mutation_log="$WORK_DIR/mutation-${block_size}.log"
    reboot_log="$WORK_DIR/reboot-${block_size}.log"

    echo "=== ext2 ${block_size}-byte blocks: generate ==="
    "$GENEXT2FS" -f -B "$block_size" -b "$blocks" -N 4096 -d "$fixture_root" "$test_image"
    check_fs "$test_image" "baseline ${block_size}-byte blocks"

    make_boot_disk "$mutation_disk" "root=root_task starter=sh ext2test=1 ext2test_exit=1 test_exit=1 test_exit_delay=$TEST_EXIT_DELAY"
    echo "=== ext2 ${block_size}-byte blocks: mutation boot ==="
    boot_guest "$mutation_disk" "$test_image" "$mutation_log"
    check_fs "$test_image" "after Robu mutation ${block_size}-byte blocks"

    make_boot_disk "$reboot_disk" "root=root_task starter=sh ext2test=1 ext2test_exit=1 test_exit=1 test_exit_delay=$TEST_EXIT_DELAY"
    echo "=== ext2 ${block_size}-byte blocks: reboot verification ==="
    boot_guest "$reboot_disk" "$test_image" "$reboot_log"
    check_fs "$test_image" "after QEMU reboot ${block_size}-byte blocks"
done

unsupported_image="$WORK_DIR/ext4-unsupported.img"
unsupported_disk="$WORK_DIR/boot-unsupported.img"
unsupported_log="$WORK_DIR/unsupported.log"
echo "=== ext4 unsupported-feature rejection ==="
"$MKE2FS" -q -F -t ext4 -b 4096 -O extent,^64bit,^metadata_csum "$unsupported_image" 32768
check_fs "$unsupported_image" "unsupported ext4 baseline"
make_boot_disk "$unsupported_disk" "root=root_task starter=sh ext2test=1 ext2test_reject=1 test_exit=1 test_exit_delay=3"
boot_rejected_image "$unsupported_disk" "$unsupported_image" "$unsupported_log"
check_fs "$unsupported_image" "after rejected ext4 boot"

echo "PASS: ext2 hardening matrix completed for $EXT2_BLOCK_SIZES"
