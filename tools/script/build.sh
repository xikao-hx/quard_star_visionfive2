#!/usr/bin/env bash

set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo_dir=$(cd -- "$script_dir/../.." && pwd)
nfs_root=${NFS_ROOT:-$repo_dir/nfs_rootfs}
default_private_key=${OTA_PRIVATE_KEY:-$repo_dir/keys/ota_private.pem}
default_output_dir=${OTA_OUTPUT_DIR:-$nfs_root/ota_package}
installed_public_key=${OTA_PUBLIC_KEY_INSTALL_PATH:-$nfs_root/etc/ota/keys/ota_public.pem}
controlled_public_key=${OTA_PUBLIC_KEY_SOURCE_PATH:-$repo_dir/conf/ota/keys/ota_public.pem}
sys_version_file=${OTA_SYS_VERSION_FILE:-$repo_dir/conf/ota/sys-version}

# shellcheck source=tools/script/ota_release.sh
. "$script_dir/ota_release.sh"

usage()
{
    cat <<'EOF'
Usage: ./tools/script/build.sh COMMAND [OPTIONS]

Commands:
  keygen [PRIVATE_KEY]  Generate an RSA-2048 OTA key pair
  package OPTIONS...    Build a signed A/B OTA package from work/target/amp

Defaults:
  private key: keys/ota_private.pem
  public key:  conf/ota/keys/ota_public.pem
  sys version: conf/ota/sys-version
  package:     nfs_rootfs/ota_package

Run keygen before building the rootfs. The package command never changes keys
and rejects a private key that does not match the controlled public key.
EOF
}

public_key_for_private()
{
    local private_key=$1
    printf '%s/ota_public.pem\n' "$(dirname -- "$private_key")"
}

generate_ota_keys()
{
    local private_key=${1:-$default_private_key}
    local public_key
    local key_description

    private_key=$(readlink -m -- "$private_key")
    public_key=$(public_key_for_private "$private_key")
    mkdir -p "$(dirname -- "$private_key")" \
        "$(dirname -- "$controlled_public_key")" \
        "$(dirname -- "$installed_public_key")"

    if [[ -e "$private_key" ]]; then
        if [[ ! -f "$private_key" ]]; then
            printf 'Private key path is not a regular file: %s\n' "$private_key" >&2
            return 1
        fi
        if ! openssl rsa -in "$private_key" -check -noout >/dev/null 2>&1; then
            printf 'OTA private key is not a valid RSA key: %s\n' "$private_key" >&2
            return 1
        fi
        printf 'Using existing OTA private key: %s\n' "$private_key"
    else
        umask 077
        openssl genpkey -quiet -algorithm RSA -pkeyopt rsa_keygen_bits:2048 \
            -out "$private_key"
        chmod 0600 "$private_key"
        printf 'Generated OTA private key: %s\n' "$private_key"
    fi

    key_description=$(openssl pkey -in "$private_key" -text -noout 2>/dev/null | sed -n '1p')
    if [[ "$key_description" != *"(2048 bit"* ]]; then
        printf 'OTA private key must be an RSA-2048 key: %s\n' "$private_key" >&2
        return 1
    fi

    openssl pkey -in "$private_key" -pubout -out "$public_key"
    install -D -m 0644 "$public_key" "$controlled_public_key"
    install -D -m 0644 "$public_key" "$installed_public_key"
    printf 'Generated OTA public key: %s\n' "$public_key"
    printf 'Updated controlled OTA public key: %s\n' "$controlled_public_key"
    printf 'Installed development OTA public key: %s\n' "$installed_public_key"
}

validate_signing_key_pair()
{
    local private_key=$1
    local private_fingerprint
    local public_fingerprint

    if [[ ! -f "$private_key" ]]; then
        printf 'OTA private key is missing: %s\n' "$private_key" >&2
        printf 'Run ./build.sh keygen before rebuilding the rootfs and package.\n' >&2
        return 1
    fi
    if [[ ! -f "$controlled_public_key" ]]; then
        printf 'Controlled OTA public key is missing: %s\n' \
            "$controlled_public_key" >&2
        return 1
    fi

    private_fingerprint=$(openssl pkey -in "$private_key" -pubout -outform DER | sha256sum)
    public_fingerprint=$(openssl pkey -pubin -in "$controlled_public_key" -outform DER | sha256sum)
    private_fingerprint=${private_fingerprint%% *}
    public_fingerprint=${public_fingerprint%% *}
    if [[ "$private_fingerprint" != "$public_fingerprint" ]]; then
        printf 'OTA private key does not match controlled public key: %s\n' \
            "$controlled_public_key" >&2
        return 1
    fi
}

option_value()
{
    local option=$1
    shift

    while (($#)); do
        case "$1" in
            "$option")
                if (($# < 2)); then
                    printf 'Option %s requires a value\n' "$option" >&2
                    return 2
                fi
                printf '%s\n' "$2"
                return 0
                ;;
            "$option="*)
                printf '%s\n' "${1#*=}"
                return 0
                ;;
        esac
        shift
    done

    return 1
}

option_count()
{
    local option=$1
    local count=0
    shift

    while (($#)); do
        case "$1" in
            "$option"|"$option="*) count=$((count + 1)) ;;
        esac
        shift
    done

    printf '%s\n' "$count"
}

validate_rootfs_release()
{
    local expected_version=$1
    local rootfs_image=$repo_dir/work/target/amp/rootfs.ext4
    local debugfs_bin=${OTA_DEBUGFS:-}
    local image_version
    local image_public_fingerprint
    local controlled_public_fingerprint

    if [[ ! -f "$rootfs_image" ]]; then
        printf 'AMP rootfs image is missing: %s\n' "$rootfs_image" >&2
        return 1
    fi
    if [[ -z "$debugfs_bin" ]]; then
        if [[ -x "$repo_dir/work/buildroot_rootfs/host/sbin/debugfs" ]]; then
            debugfs_bin=$repo_dir/work/buildroot_rootfs/host/sbin/debugfs
        else
            debugfs_bin=$(command -v debugfs || true)
        fi
    fi
    if [[ -z "$debugfs_bin" || ! -x "$debugfs_bin" ]]; then
        printf 'debugfs is required to verify the rootfs release version\n' >&2
        return 1
    fi

    image_version=$("$debugfs_bin" -R 'cat /etc/version' "$rootfs_image" 2>/dev/null | \
        sed -n '1{s/\r$//;p;}')
    if [[ "$image_version" != "$expected_version" ]]; then
        printf 'AMP rootfs version %s does not match controlled version %s\n' \
            "${image_version:-<missing>}" "$expected_version" >&2
        printf 'Rebuild the AMP rootfs before creating the OTA package.\n' >&2
        return 1
    fi

    if ! image_public_fingerprint=$( \
        "$debugfs_bin" -R 'cat /etc/ota/keys/ota_public.pem' "$rootfs_image" 2>/dev/null | \
        openssl pkey -pubin -outform DER 2>/dev/null | sha256sum); then
        printf 'AMP rootfs does not contain a valid OTA public key\n' >&2
        return 1
    fi
    controlled_public_fingerprint=$( \
        openssl pkey -pubin -in "$controlled_public_key" -outform DER | sha256sum)
    image_public_fingerprint=${image_public_fingerprint%% *}
    controlled_public_fingerprint=${controlled_public_fingerprint%% *}
    if [[ "$image_public_fingerprint" != "$controlled_public_fingerprint" ]]; then
        printf 'AMP rootfs OTA public key does not match controlled public key\n' >&2
        printf 'Rebuild the AMP rootfs after running keygen.\n' >&2
        return 1
    fi
}

build_ota_package()
{
    local private_key
    local output_dir
    local option_status
    local resolved_output_dir
    local resolved_nfs_root
    local controlled_version
    local requested_version
    local sys_version_option_count
    local -a package_args=("$@")

    for option in "$@"; do
        if [[ "$option" == "-h" || "$option" == "--help" ]]; then
            python3 "$script_dir/build_ota_package.py" --help
            return 0
        fi
    done

    controlled_version=$(ota_read_sys_version "$sys_version_file")
    sys_version_option_count=$(option_count --sys-version "$@")
    if ((sys_version_option_count > 1)); then
        printf 'Option --sys-version may be specified only once\n' >&2
        return 2
    fi
    option_status=0
    requested_version=$(option_value --sys-version "$@") || option_status=$?
    case "$option_status" in
        0)
            if [[ "$requested_version" != "$controlled_version" ]]; then
                printf 'Package sys-version %s does not match controlled rootfs version %s\n' \
                    "$requested_version" "$controlled_version" >&2
                return 1
            fi
            ;;
        1)
            package_args+=(--sys-version "$controlled_version")
            ;;
        *) return "$option_status" ;;
    esac
    validate_rootfs_release "$controlled_version"

    option_status=0
    private_key=$(option_value --private-key "$@") || option_status=$?
    case "$option_status" in
        0) ;;
        1)
            private_key=$default_private_key
            package_args+=(--private-key "$private_key")
            ;;
        *) return "$option_status" ;;
    esac

    option_status=0
    output_dir=$(option_value --output-dir "$@") || option_status=$?
    case "$option_status" in
        0) ;;
        1)
            output_dir=$default_output_dir
            package_args+=(--output-dir "$output_dir")
            ;;
        *) return "$option_status" ;;
    esac

    if [[ -z "$private_key" || -z "$output_dir" ]]; then
        printf 'Private-key and output directory values must not be empty\n' >&2
        return 2
    fi

    validate_signing_key_pair "$private_key"

    resolved_output_dir=$(readlink -m -- "$output_dir")
    resolved_nfs_root=$(readlink -m -- "$nfs_root")
    case "$resolved_output_dir" in
        /|"$repo_dir"|"$resolved_nfs_root")
            printf 'Refusing to replace broad output directory: %s\n' "$resolved_output_dir" >&2
            return 1
            ;;
    esac

    if [[ -e "$resolved_output_dir" || -L "$resolved_output_dir" ]]; then
        printf 'Removing previous OTA package: %s\n' "$resolved_output_dir"
        rm -rf -- "$resolved_output_dir"
    fi

    python3 "$script_dir/build_ota_package.py" \
        --project-root "$repo_dir" \
        --auto-from-target \
        "${package_args[@]}"
}

command=${1:-help}
if (($#)); then
    shift
fi

case "$command" in
    help|-h|--help)
        usage
        ;;
    keygen)
        if (($# > 1)); then
            printf 'keygen accepts at most one private-key path\n' >&2
            exit 2
        fi
        generate_ota_keys "${1:-$default_private_key}"
        ;;
    package)
        build_ota_package "$@"
        ;;
    *)
        printf 'Unknown command: %s\n\n' "$command" >&2
        usage >&2
        exit 2
        ;;
esac
