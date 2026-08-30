#!/bin/bash

set -e

SHELL_FOLDER=$(cd "$(dirname "$0")"; pwd)
PROJECT_ROOT=$(cd "$SHELL_FOLDER/../.."; pwd)
export ARCH=riscv
KERN_DIR=${KERN_DIR:-$PROJECT_ROOT/linux}
KBUILD_OUTPUT=${KBUILD_OUTPUT:-$PROJECT_ROOT/work/linux}
TOOLCHAIN_BIN=${TOOLCHAIN_BIN:-$PROJECT_ROOT/work/buildroot_initramfs/host/bin}
CROSS_COMPILE=${CROSS_COMPILE:-$TOOLCHAIN_BIN/riscv64-buildroot-linux-gnu-}
export KERN_DIR KBUILD_OUTPUT CROSS_COMPILE
export PATH="$TOOLCHAIN_BIN:$PATH"

# The board mounts this directory directly at /mnt.
NFS_ROOTFS=${NFS_ROOTFS:-$PROJECT_ROOT/nfs_rootfs/mailbox}
choice=${1:-build}

mkdir -p "$NFS_ROOTFS"

if [[ "$choice" == "clear" ]]; then
    rm -f "$NFS_ROOTFS"/*.ko
    choice=clean
fi

if [[ "$choice" == "clean" ]]; then
    make -C "$SHELL_FOLDER/mailbox" clean
    make -C "$SHELL_FOLDER/consumer" clean
    exit 0
fi

make -C "$SHELL_FOLDER/mailbox" modules
make -C "$SHELL_FOLDER/consumer" modules
rm -f "$NFS_ROOTFS"/*.ko

cp "$SHELL_FOLDER/mailbox/starfive_ipi_mailbox.ko" "$NFS_ROOTFS/"
cp "$SHELL_FOLDER/mailbox/starfive_ipi_mailbox-test.ko" "$NFS_ROOTFS/"
cp "$SHELL_FOLDER/consumer/quard_mbox_router.ko" "$NFS_ROOTFS/"
cp "$SHELL_FOLDER/consumer/quard_remote_console.ko" "$NFS_ROOTFS/"
cp "$SHELL_FOLDER/consumer/quard_log.ko" "$NFS_ROOTFS/"
cp "$SHELL_FOLDER/consumer/quard_nor_client.ko" "$NFS_ROOTFS/"
cp "$SHELL_FOLDER/minicom-start.sh" "$NFS_ROOTFS/"
cp "$SHELL_FOLDER/install.sh" "$NFS_ROOTFS/"

echo "IPI mailbox modules installed in: $NFS_ROOTFS"
ls -l "$NFS_ROOTFS/starfive_ipi_mailbox.ko" \
      "$NFS_ROOTFS/starfive_ipi_mailbox-test.ko" \
      "$NFS_ROOTFS/quard_mbox_router.ko" \
      "$NFS_ROOTFS/quard_remote_console.ko" \
      "$NFS_ROOTFS/quard_log.ko" \
      "$NFS_ROOTFS/quard_nor_client.ko"
