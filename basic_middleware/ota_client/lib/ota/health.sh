PROC_MOUNTS=${OTA_CLIENT_PROC_MOUNTS:-/proc/mounts}
PROC_CMDLINE=${OTA_CLIENT_PROC_CMDLINE:-/proc/cmdline}
USERDATA_MOUNTPOINT=${OTA_CLIENT_USERDATA_MOUNTPOINT:-/userdata}
DISK_BY_PARTLABEL_DIR=${OTA_CLIENT_DISK_BY_PARTLABEL_DIR:-/dev/disk/by-partlabel}
SYS_CLASS_BLOCK_DIR=${OTA_CLIENT_SYS_CLASS_BLOCK_DIR:-/sys/class/block}
READLINK_BIN=${OTA_CLIENT_READLINK_BIN:-readlink}
BLKID_BIN=${OTA_CLIENT_BLKID_BIN:-blkid}

mount_source_for()
{
    mount_path=$1
    awk -v wanted="$mount_path" '$2 == wanted { print $1; exit }' "$PROC_MOUNTS"
}

canonical_device()
{
    "$READLINK_BIN" -f "$1" 2>/dev/null
}

device_property()
{
    property=$1
    device=$2
    "$BLKID_BIN" -s "$property" -o value "$device" 2>/dev/null | sed -n '1p'
}

device_partition_number()
{
    device_name=${1##*/}
    partition_file=$SYS_CLASS_BLOCK_DIR/$device_name/partition
    [ -r "$partition_file" ] || return 1
    sed -n '1p' "$partition_file"
}

cmdline_value()
{
    wanted=$1
    for item in $(cat "$PROC_CMDLINE" 2>/dev/null); do
        case "$item" in
            "$wanted"=*)
                printf '%s\n' "${item#*=}"
                return 0
                ;;
        esac
    done
    return 1
}

normalize_bank()
{
    case "$1" in
        0|a|A) printf 'a\n' ;;
        1|b|B) printf 'b\n' ;;
        *) return 1 ;;
    esac
}

recovery_value()
{
    key=$1
    printf '%s\n' "$RECOVERY_DUMP" | awk -F= -v wanted="$key" '
        $1 == wanted {
            value = $2
            sub(/[[:space:]].*$/, "", value)
            print value
            exit
        }
    '
}

require_userdata_health()
{
    if [ ! -r "$PROC_MOUNTS" ]; then
        error "stage=health_check event=failed reason=mount_table_unavailable path=$PROC_MOUNTS"
        return 1
    fi

    userdata_source=$(mount_source_for "$USERDATA_MOUNTPOINT")
    if [ -z "$userdata_source" ]; then
        error "stage=health_check event=failed reason=userdata_not_mounted mountpoint=$USERDATA_MOUNTPOINT"
        return 1
    fi

    userdata_label=$(device_property PARTLABEL "$userdata_source")
    if [ "$userdata_label" != userdata ]; then
        error "stage=health_check event=failed reason=userdata_label_mismatch source=$userdata_source label=${userdata_label:-unknown}"
        return 1
    fi

    if [ ! -d "$USERDATA_MOUNTPOINT/ota" ] && \
            ! mkdir -p "$USERDATA_MOUNTPOINT/ota"; then
        error "stage=health_check event=failed reason=userdata_ota_create_failed path=$USERDATA_MOUNTPOINT/ota"
        return 1
    fi
    if [ ! -w "$USERDATA_MOUNTPOINT/ota" ]; then
        error "stage=health_check event=failed reason=userdata_ota_unavailable path=$USERDATA_MOUNTPOINT/ota"
        return 1
    fi

    stage_log "health_check" "success" "check=userdata source=$userdata_source label=$userdata_label"
}

run_health_hooks()
{
    [ -d "$HEALTH_CHECK_DIR" ] || return 0

    for health_hook in "$HEALTH_CHECK_DIR"/*; do
        [ -e "$health_hook" ] || continue
        [ -x "$health_hook" ] || continue
        if ! "$health_hook"; then
            error "stage=health_check event=failed reason=health_hook_failed hook=$health_hook"
            return 1
        fi
        stage_log "health_check" "success" "check=health_hook hook=$health_hook"
    done
}

run_boot_health_gate()
{
    require_userdata_health || return 1

    if [ ! -f "$OTA_PACKAGE_DIR/data.json" ]; then
        error "stage=health_check event=failed reason=manifest_missing path=$OTA_PACKAGE_DIR/data.json"
        return 1
    fi

    RECOVERY_DUMP=$("$OTA_INFO_BIN" dump-state 2>/dev/null) || {
        error "stage=health_check event=failed reason=recovery_unavailable"
        return 1
    }
    recovery_current=$(normalize_bank "$(recovery_value current_bank)") || {
        error "stage=health_check event=failed reason=current_bank_invalid"
        return 1
    }
    recovery_target=$(normalize_bank "$(recovery_value target_bank)") || {
        error "stage=health_check event=failed reason=target_bank_invalid"
        return 1
    }
    if [ "$recovery_current" != "$recovery_target" ]; then
        error "stage=health_check event=failed reason=recovery_bank_mismatch current_bank=$recovery_current target_bank=$recovery_target"
        return 1
    fi

    cmdline_bank=$(normalize_bank "$(cmdline_value active_bank)") || {
        error "stage=health_check event=failed reason=active_bank_missing_or_invalid"
        return 1
    }
    if [ "$cmdline_bank" != "$recovery_current" ]; then
        error "stage=health_check event=failed reason=active_bank_mismatch active_bank=$cmdline_bank current_bank=$recovery_current"
        return 1
    fi

    expected_root_label=rootfs_$recovery_current
    expected_root_path=$DISK_BY_PARTLABEL_DIR/$expected_root_label
    root_source=$(mount_source_for /)
    actual_root_device=$(canonical_device "$root_source")
    if [ -z "$root_source" ] || [ -z "$actual_root_device" ]; then
        error "stage=health_check event=failed reason=rootfs_device_unavailable source=${root_source:-unknown}"
        return 1
    fi

    root_partlabel=$(device_property PARTLABEL "$actual_root_device")
    root_identity=partlabel
    if [ -n "$root_partlabel" ]; then
        if [ "$root_partlabel" != "$expected_root_label" ]; then
            error "stage=health_check event=failed reason=rootfs_label_mismatch source=$root_source label=$root_partlabel expected_label=$expected_root_label"
            return 1
        fi
        if [ -e "$expected_root_path" ]; then
            expected_root_device=$(canonical_device "$expected_root_path")
            if [ -z "$expected_root_device" ] || \
                    [ "$actual_root_device" != "$expected_root_device" ]; then
                error "stage=health_check event=failed reason=rootfs_device_mismatch source=$root_source expected_label=$expected_root_label"
                return 1
            fi
        fi
    else
        case "$recovery_current" in
            a) expected_root_partition=4 ;;
            b) expected_root_partition=6 ;;
        esac
        root_partition=$(device_partition_number "$actual_root_device" 2>/dev/null || true)
        if [ "$root_partition" != "$expected_root_partition" ]; then
            error "stage=health_check event=failed reason=rootfs_legacy_partition_mismatch source=$root_source partition=${root_partition:-unknown} expected_partition=$expected_root_partition"
            return 1
        fi
        root_identity=legacy_partition
        root_partlabel=$expected_root_label
        stage_log "health_check" "compatibility" \
            "reason=partlabel_missing source=$root_source partition=$root_partition expected_label=$expected_root_label"
    fi

    root_partuuid=$(device_property PARTUUID "$actual_root_device")
    cmdline_root=$(cmdline_value root 2>/dev/null || true)
    case "$cmdline_root" in
        PARTUUID=*) cmdline_partuuid=${cmdline_root#PARTUUID=} ;;
        *)
            error "stage=health_check event=failed reason=root_partuuid_missing root=${cmdline_root:-unknown}"
            return 1
            ;;
    esac
    if [ "$root_partlabel" != "$expected_root_label" ] ||
            [ "$(printf '%s' "$root_partuuid" | tr '[:upper:]' '[:lower:]')" != \
              "$(printf '%s' "$cmdline_partuuid" | tr '[:upper:]' '[:lower:]')" ]; then
        error "stage=health_check event=failed reason=rootfs_identity_mismatch label=${root_partlabel:-unknown} expected_label=$expected_root_label partuuid=${root_partuuid:-unknown} cmdline_partuuid=${cmdline_partuuid:-unknown}"
        return 1
    fi

    manifest_version=$(extract_json_string sys_version "$OTA_PACKAGE_DIR/data.json")
    if [ -z "$manifest_version" ] || [ "$CURRENT_VERSION" != "$TARGET_VERSION" ] ||
            [ "$CURRENT_VERSION" != "$manifest_version" ]; then
        error "stage=health_check event=failed reason=version_mismatch current_version=${CURRENT_VERSION:-unknown} target_version=${TARGET_VERSION:-unknown} manifest_version=${manifest_version:-unknown}"
        return 1
    fi

    run_health_hooks || return 1
    stage_log "health_check" "success" \
        "current_bank=$recovery_current target_bank=$recovery_target root_identity=$root_identity root_label=$root_partlabel root_partuuid=$root_partuuid version=$CURRENT_VERSION"
}
