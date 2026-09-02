#!/bin/sh

set -e
cd "$(dirname "$0")/.."

IMG=$(mktemp /tmp/diskfs-persist-XXXXXX.img)
LOG1=$(mktemp /tmp/diskfs-persist-boot1-XXXXXX.log)
LOG2=$(mktemp /tmp/diskfs-persist-boot2-XXXXXX.log)
trap 'rm -f "$IMG" "$LOG1" "$LOG2"' EXIT

truncate -s 16M "$IMG"

make all > /dev/null 2>&1
rm -f build/bootloader/ext2-kernel-debugfs-script.txt build/bootloader/ext2-kernel-userland-script.txt \
    build/bootloader/ext2-kernel.img build/robu-kernel-disk.img
make bootloader-kernel-disk \
    QEMU_APPEND="root=root_task starter=hello_initsys test_exit=0 test_exit_delay=35" \
    > /dev/null 2>&1

echo "=== boot 1: creating /mnt/disk0/persist.txt ==="
{ sleep 45; printf 'touch /mnt/disk0/persist.txt\n'; sleep 8; } | \
    make run QEMU_DISK="$IMG" \
        > "$LOG1" 2>&1 || true

if ! grep -q 'touch /mnt/disk0/persist.txt' "$LOG1"; then
    echo "FAIL: boot 1 never appears to have received the touch command (see $LOG1)"
    cat "$LOG1"
    exit 1
fi
if ! grep -q '\[boot\] exiting with code 0' "$LOG1"; then
    echo "FAIL: boot 1 did not exit cleanly via isa-debug-exit (see $LOG1)"
    exit 1
fi
echo "DISKFS_WROTE_OK"

echo "=== boot 2: verifying /mnt/disk0/persist.txt survived ==="
{ sleep 45; printf 'ls /mnt/disk0\n'; sleep 8; } | \
    make run QEMU_DISK="$IMG" \
        > "$LOG2" 2>&1 || true

if ! grep -q '\[boot\] exiting with code 0' "$LOG2"; then
    echo "FAIL: boot 2 did not exit cleanly via isa-debug-exit (see $LOG2)"
    exit 1
fi
if ! grep -q 'persist\.txt' "$LOG2"; then
    echo "FAIL: persist.txt not found in boot 2's \`ls /mnt/disk0\` output (see $LOG2)"
    exit 1
fi
echo "DISKFS_VERIFY_OK"
echo "=== PASS: diskfs persisted a file across two clean boots ==="
