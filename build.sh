#!/usr/bin/env bash

set -euo pipefail

repo_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
jobs=${JOBS:-$(nproc)}
variant=${VARIANT:-userdebug}
rtos=${RTOS:-freertos}
RVROOT=${RVROOT:-$repo_dir/work/buildroot_initramfs/host}
RISCV=${RISCV:-$RVROOT}
CROSS_COMPILE=${CROSS_COMPILE:-$RVROOT/bin/riscv64-buildroot-linux-gnu-}
TRUSTED_CROSS_COMPILE=${TRUSTED_CROSS_COMPILE:-/opt/riscv/bin/riscv64-unknown-elf-}
export RVROOT RISCV CROSS_COMPILE TRUSTED_CROSS_COMPILE
export PATH="$RVROOT/bin:$PATH"

usage()
{
    cat <<'EOF'
Usage: ./build.sh COMMAND [ACTION|OPTIONS]

Commands:
  sdk build [normal|amp|all] Build selected SDK images (default: all)
  sdk clean                  Remove only published images under work/target
  bsp [build|clean|clear]    Build/install IPI mailbox BSP modules
  app [build|clean|clear]    Build/install applications
  ota [build|clean|install|all]
                             Build OTA info, package and client components
  tools <gpt|recovery|all>   Generate NOR images/reports in work/target/amp
  keygen [PRIVATE_KEY]       Generate and install the OTA signing key pair
  package OPTIONS...         Build a signed OTA package in nfs_rootfs
  all                        Build SDK, BSP, app and OTA components

Package example:
  ./build.sh keygen
  ./build.sh package --sys-version 1.2.4 \
    --rollback-index 10204 --session-id 20260908-001 \
    --target-bank b --include-stage2

Environment:
  JOBS=<n>                   Parallel SDK build jobs (default: nproc)
  VARIANT=user|userdebug|eng OTA component build variant
  RTOS=freertos|rtthread      RTOS included in the AMP firmware (default: freertos)
  RVROOT=<path>              Buildroot host/toolchain directory
  CROSS_COMPILE=<prefix>     Component cross-compiler prefix
  TRUSTED_CROSS_COMPILE=<prefix>
                             FreeRTOS bare-metal compiler prefix
EOF
}

build_userdata_image()
{
    local userdata_image="$repo_dir/work/userdata.ext4"
    local temp_image="${userdata_image}.tmp"
    local mke2fs="$repo_dir/work/buildroot_rootfs/host/sbin/mkfs.ext4"

    if [[ ! -x "$mke2fs" ]]; then
        printf 'userdata image tool is missing: %s\n' "$mke2fs" >&2
        return 1
    fi

    mkdir -p "$repo_dir/work"
    if ! truncate -s 2048M "$temp_image" ||
       ! "$mke2fs" -F -t ext4 -L userdata "$temp_image"; then
        rm -f -- "$temp_image"
        return 1
    fi
    mv -f -- "$temp_image" "$userdata_image"
    printf 'Userdata image generated: %s (2 GiB)\n' "$userdata_image"
}

run_sdk()
{
    local action=${1:-build}
    local image_set=${2:-all}

    if (($# > 2)); then
        printf 'sdk accepts: sdk build [normal|amp|all], or sdk clean\n' >&2
        return 2
    fi

    case "$action" in
        build)
            case "$image_set" in
                normal)
                    make -C "$repo_dir" publish_normal_images -j"$jobs"
                    ;;
                amp)
                    make -C "$repo_dir" publish_amp_images RTOS="$rtos" -j"$jobs"
                    build_userdata_image
                    "$repo_dir/tools/image/build.sh" all
                    ;;
                all)
                    make -C "$repo_dir" publish_all_images RTOS="$rtos" -j"$jobs"
                    build_userdata_image
                    "$repo_dir/tools/image/build.sh" all
                    ;;
                *)
                    printf 'Unknown sdk image set: %s (expected normal, amp or all)\n' \
                        "$image_set" >&2
                    return 2
                    ;;
            esac
            ;;
        clean)
            if (($# > 1)); then
                printf 'sdk clean does not accept an image set\n' >&2
                return 2
            fi
            make -C "$repo_dir" publish-clean
            ;;
        *)
            printf 'Unknown sdk action: %s\n' "$action" >&2
            return 2
            ;;
    esac
}

run_bsp()
{
    "$repo_dir/bsp/ipi_mailbox/build.sh" "${1:-build}"
}

run_app()
{
    "$repo_dir/app/build.sh" "${1:-build}"
}

run_ota()
{
    local action=${1:-build}
    local flag

    case "$action" in
        build|clean|install|all) flag="--$action" ;;
        *)
            printf 'Unknown ota action: %s\n' "$action" >&2
            exit 2
            ;;
    esac

    "$repo_dir/basic_middleware/ota_info/build.sh" "variant=$variant" "$flag"
    "$repo_dir/basic_middleware/ota_package/build.sh" "variant=$variant" "$flag"
    # if [[ "$action" == "build" || "$action" == "install" || "$action" == "all" ]]; then
    #     "$repo_dir/basic_middleware/ota_client/build.sh"
    # fi
}

run_keygen()
{
    "$repo_dir/tools/script/build.sh" keygen "$@"
}

run_package()
{
    "$repo_dir/tools/script/build.sh" package "$@"
}

command=${1:-help}
if [[ $# -gt 0 ]]; then
    shift
fi

case "$command" in
    help|-h|--help)
        usage
        ;;
    sdk)
        run_sdk "$@"
        ;;
    bsp)
        run_bsp "${1:-build}"
        ;;
    app)
        run_app "${1:-build}"
        ;;
    ota)
        run_ota "${1:-build}"
        ;;
    tools)
        if [[ $# -ne 1 ]]; then
            printf 'tools requires exactly one target: gpt, recovery or all\n' >&2
            exit 2
        fi
        "$repo_dir/tools/image/build.sh" "$1"
        ;;
    keygen)
        run_keygen "$@"
        ;;
    package)
        run_package "$@"
        ;;
    all)
        if [[ $# -ne 0 ]]; then
            printf 'all does not accept additional arguments\n' >&2
            exit 2
        fi
        run_sdk build all
        run_bsp build
        run_app build
        run_ota build
        ;;
    *)
        printf 'Unknown command: %s\n\n' "$command" >&2
        usage >&2
        exit 2
        ;;
esac
