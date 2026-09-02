#!/bin/sh
set -eu

source_dir=$1
config=$source_dir/.config
hostcc=${HOSTCC:-clang}

patch -d "$source_dir" -p1 < "$(dirname "$0")/../apps/busybox-robu/disable-pidof-exe.patch"
make -C "$source_dir" HOSTCC="$hostcc" allnoconfig >/dev/null

for option in BUSYBOX STATIC LONG_OPTS AWK BASE64 BASENAME BLKID CAT CKSUM CLEAR CMP CP CUT DATE DD DIRNAME DU ECHO ENV EXPAND FACTOR FALSE FEATURE_BLKID_TYPE FEATURE_FAST_TOP FEATURE_VOLUMEID_EXT FEATURE_VOLUMEID_FAT FIND FOLD FREE GREP HEAD HEXDUMP HOSTNAME KILL LINK LS MD5SUM MKDIR MKNOD MV NOHUP OD PASTE PGREP PIDOF PKILL PRINTF PS PSTREE PWD RM RMDIR SED SEQ SHA1SUM SHA256SUM SHA512SUM SLEEP SORT STAT SYNC TAIL TEE TEST TOUCH TR TRUE TTY UNEXPAND UNIQ UNLINK WC WHOAMI XARGS YES; do
    sed "s/^# CONFIG_${option} is not set$/CONFIG_${option}=y/" "$config" > "$config.next"
    mv "$config.next" "$config"
done

yes '' | make -C "$source_dir" HOSTCC="$hostcc" oldconfig >/dev/null
