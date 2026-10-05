#!/bin/bash

# Usage: ./build.sh variant=<user|userdebug|eng> --<clean|build|all|install>

VARIANT="userdebug"
ACTION="build"

TOP_DIR=$(cd "$(dirname "$0")"; pwd)
BUILD_DIR=${TOP_DIR}/build
REPO_ROOT=$(readlink -f "${TOP_DIR}/../..")
TOOLCHAIN_PREFIX=${CROSS_COMPILE:-${REPO_ROOT}/work/buildroot_initramfs/host/bin/riscv64-buildroot-linux-gnu-}
OTA_INFO_DIR=${TOP_DIR}/../ota_info
NFS_ROOT=${NFS_ROOT:-${REPO_ROOT}/nfs_rootfs}
TARGET_SYSROOT=${TARGET_SYSROOT:-${REPO_ROOT}/work/buildroot_rootfs/host/riscv64-buildroot-linux-gnu/sysroot}

for arg in "$@"; do
    case $arg in
        variant=user|variant=userdebug|variant=eng)
            VARIANT="${arg#variant=}"
            ;;
        --clean)
            ACTION="clean"
            ;;
        --build)
            ACTION="build"
            ;;
        --all)
            ACTION="all"
            ;;
        --install)
            ACTION="install"
            ;;
        *)
            echo "Unknown args: $arg"
            echo "Usage: ./build.sh variant=<user|userdebug|eng> --<clean|build|all|install>"
            exit 1
            ;;
    esac
done

MAKE_EXTRA_ARGS=""

case $VARIANT in
    user|userdebug|eng)
        ;;
    *)
        echo "variant: $VARIANT"
        exit 1
        ;;
esac

if [ -x "${TOOLCHAIN_PREFIX}gcc" ]; then
    MAKE_EXTRA_ARGS="${MAKE_EXTRA_ARGS} CROSS_COMPILE=${TOOLCHAIN_PREFIX}"
fi

set -euo pipefail

do_clean() {
    echo "clean..."
    make -C "${TOP_DIR}" ${MAKE_EXTRA_ARGS} clean
    echo "clean finished."
}

do_build() {
    echo "build start, variant=$VARIANT..."
    make -C "${TOP_DIR}" ${MAKE_EXTRA_ARGS}
    echo "build finished."
}

do_install() {
    echo "start install..."
    mkdir -p "${NFS_ROOT}/bin" "${NFS_ROOT}/lib"
    cp -f "${BUILD_DIR}/ota_package" "${NFS_ROOT}/bin/"
    cp -f "${OTA_INFO_DIR}/build"/libbh_ota.so* "${NFS_ROOT}/lib/"
    cp -Lf "${TARGET_SYSROOT}/usr/lib/libcrypto.so.1.1" "${NFS_ROOT}/lib/"
    echo "install finished."
}

case $ACTION in
    clean)
        do_clean
        ;;
    build)
        do_build
        ;;
    install)
        do_install
        ;;
    all)
        do_clean
        do_build
        do_install
        ;;
    *)
        echo "unknown operation: $ACTION"
        exit 1
        ;;
esac

exit 0
