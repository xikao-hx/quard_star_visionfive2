#!/bin/bash

# Usabe: ./build.sh variant=<user|userdebug|eng> --<clean|build|all|install>

VARIANT="userdebug"
ACTION="build"
SOURCE_DIR=$(dirname "$(readlink -f "$0")")

TOP_DIR=${SOURCE_DIR}
BUILD_DIR=${TOP_DIR}/build
REPO_ROOT=$(readlink -f "${SOURCE_DIR}/../..")
TOOLCHAIN_PREFIX=${CROSS_COMPILE:-${REPO_ROOT}/work/buildroot_initramfs/host/bin/riscv64-buildroot-linux-gnu-}
NFS_ROOT=${NFS_ROOT:-${REPO_ROOT}/nfs_rootfs}

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
    user)
        MAKE_EXTRA_ARGS=""
        BUILD_TYPE="Release"
        ;;
    userdebug)
        MAKE_EXTRA_ARGS=""
        BUILD_TYPE="RelWithDebInfo"
        ;;
    eng)
        MAKE_EXTRA_ARGS=""
        BUILD_TYPE="Debug"
        ;;
    *)
        echo "variant: $VARIANT"
        exit 1
        ;;
esac

if [ -x "${TOOLCHAIN_PREFIX}gcc" ]; then
    MAKE_EXTRA_ARGS="${MAKE_EXTRA_ARGS} CROSS_COMPILE=${TOOLCHAIN_PREFIX}"
fi

do_clean() {
    echo "clean..."
	make -C "${SOURCE_DIR}" ${MAKE_EXTRA_ARGS} clean
    echo "clean finished."
}

do_build() {
    echo "build start, variant=$VARIANT..."
    make -C "${SOURCE_DIR}" ${MAKE_EXTRA_ARGS}
    echo "build finished."
}

set -euo pipefail

function validate_install_paths() {
    local missing_vars=()
    local required_vars=(
        "INST_CONFDIR"
        "INST_BINDIR"
        "INST_SYMBOLDIR"
        "INST_INCLUDEDIR"
        "INST_LIBDIR"
        "INST_MODULEDIR"
        "INST_CODEDIR"
        "INST_SAMPLEDIR"
    )

    for var in "${required_vars[@]}"; do
        if [ -z "${!var+x}" ] || [ -z "${!var}" ]; then
            missing_vars+=("$var")
        fi
    done

    if [ ${#missing_vars[@]} -gt 0 ]; then
        echo "error: missing installation path" >&2
        printf "  - %s\n" "${missing_vars[@]}" >&2
        exit 1
    fi

    local writable_paths=(
        "$INST_BINDIR"
        "$INST_LIBDIR"
    )

    for path in "${writable_paths[@]}"; do
        if [ ! -w "$path" ]; then
            echo "error: the path $path is unwritable" >&2
            exit 1
        fi
    done
}

do_install() {
    echo "start install..."
    # TODO：read env variable
    # validate_install_paths

    # install to nfs_rootfs/mailbox
    mkdir -p "${NFS_ROOT}/ota" "${NFS_ROOT}/bin" "${NFS_ROOT}/lib"
    cp -f "${TOP_DIR}/create_link.sh" "${NFS_ROOT}/ota/"
    cp -f "${BUILD_DIR}/ota_info" "${NFS_ROOT}/bin/"
    cp -f "${BUILD_DIR}"/libbh_ota.so* "${NFS_ROOT}/lib/"

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
