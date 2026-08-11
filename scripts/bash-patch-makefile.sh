#!/bin/sh
set -e

MF="$1"
LD_SCRIPT="$2"
CRT1="$3"
LIBC_A="$4"
LIBM_A="$5"

if ! grep -q -- '--start-group' "$MF"; then
    sed -i.bak "s#^LIBS = .*#& -Wl,--start-group $LIBC_A $LIBM_A -Wl,--end-group#" "$MF"
fi

if ! grep -q -- '-nostdlib' "$MF"; then
    sed -i.bak "s#^LDFLAGS = .*#& -nostdlib -static -fuse-ld=lld -T $LD_SCRIPT $CRT1#" "$MF"
fi

rm -f "$MF.bak"
