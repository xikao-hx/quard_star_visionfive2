#!/bin/bash

set -euo pipefail

SHELL_FOLDER=$(cd "$(dirname "$0")"; pwd)
PROJECT_ROOT=$(cd "$SHELL_FOLDER/.."; pwd)
TOOLCHAIN_BIN=${TOOLCHAIN_BIN:-$PROJECT_ROOT/work/buildroot_initramfs/host/bin}
CROSS_COMPILE=${CROSS_COMPILE:-$TOOLCHAIN_BIN/riscv64-buildroot-linux-gnu-}
NFS_ROOTFS=${NFS_ROOTFS:-$PROJECT_ROOT/nfs_rootfs/mailbox}
choice=${1:-build}

if [[ "$choice" == "clear" ]]; then
    rm -f "$NFS_ROOTFS/soc_logd"
    choice=clean
fi

if [[ "$choice" == "clean" ]]; then
    make -C "$SHELL_FOLDER/soc_log" clean
    exit 0
fi

mkdir -p "$NFS_ROOTFS"
make -C "$SHELL_FOLDER/soc_log" CROSS_COMPILE="$CROSS_COMPILE"
cp "$SHELL_FOLDER/soc_log/soc_logd" "$NFS_ROOTFS/soc_logd"
chmod 0755 "$NFS_ROOTFS/soc_logd"

echo "SoC log daemon installed in: $NFS_ROOTFS/soc_logd"
