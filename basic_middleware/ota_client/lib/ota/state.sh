OTA_INFO_BIN=${OTA_CLIENT_OTA_INFO_BIN:-/bin/ota_info}
DEFAULT_PACKAGE_DIR=${OTA_CLIENT_PACKAGE_DIR:-/userdata/ota/ota_package}
DEFAULT_PUBLIC_KEY_OVERRIDE=${OTA_CLIENT_PUBLIC_KEY_OVERRIDE:-}

set_runtime_paths()
{
    OTA_PACKAGE_DIR=$DEFAULT_PACKAGE_DIR
    if [ -n "$DEFAULT_PUBLIC_KEY_OVERRIDE" ]; then
        PUBLIC_KEY_PATH=$DEFAULT_PUBLIC_KEY_OVERRIDE
    fi

    STATE_DIR=$DOWNLOAD_DIR
    RELEASE_STATE_FILE=$STATE_DIR/current_release.env
    PACKAGE_STATE_FILE=$OTA_PACKAGE_DIR/.ota_remote_release.env
    METRICS_FILE=$STATE_DIR/ota_metrics.log
}

set_release_paths()
{
    archive_name=$(strip_file_basename "$PACKAGE_URL")
    if [ -z "$archive_name" ]; then
        archive_name=${RELEASE_ID}.tar.gz
    fi
    ARCHIVE_PATH=$DOWNLOAD_DIR/$archive_name
    PARTIAL_ARCHIVE_PATH=$ARCHIVE_PATH.part
}

save_release_state()
{
    state_file=$1
    mkdir -p "$(dirname "$state_file")" 2>/dev/null || true
    cat > "$state_file" <<EOF
release_id=$RELEASE_ID
board_type=$RELEASE_BOARD_TYPE
target_version=$TARGET_VERSION
package_url=$PACKAGE_URL
package_sha256=$PACKAGE_SHA256
session_id=$SESSION_ID
source_version=$SOURCE_VERSION
download_reported=$DOWNLOAD_REPORTED
archive_path=$ARCHIVE_PATH
package_dir=$OTA_PACKAGE_DIR
EOF
}

persist_release_state()
{
    save_release_state "$RELEASE_STATE_FILE"
    if [ -d "$OTA_PACKAGE_DIR" ]; then
        save_release_state "$PACKAGE_STATE_FILE"
    fi
}

load_release_state_from()
{
    state_file=$1
    saved_package_dir=

    if [ ! -f "$state_file" ]; then
        return 1
    fi

    RELEASE_ID=$(trim_value "$(read_kv "$state_file" "release_id")")
    RELEASE_BOARD_TYPE=$(trim_value "$(read_kv "$state_file" "board_type")")
    TARGET_VERSION=$(trim_value "$(read_kv "$state_file" "target_version")")
    PACKAGE_URL=$(trim_value "$(read_kv "$state_file" "package_url")")
    PACKAGE_SHA256=$(trim_value "$(read_kv "$state_file" "package_sha256")")
    SESSION_ID=$(trim_value "$(read_kv "$state_file" "session_id")")
    SOURCE_VERSION=$(trim_value "$(read_kv "$state_file" "source_version")")
    DOWNLOAD_REPORTED=$(trim_value "$(read_kv "$state_file" "download_reported")")
    ARCHIVE_PATH=$(trim_value "$(read_kv "$state_file" "archive_path")")
    saved_package_dir=$(trim_value "$(read_kv "$state_file" "package_dir")")

    [ -n "$RELEASE_ID" ] || return 1
    [ -n "$TARGET_VERSION" ] || return 1
    [ -n "$PACKAGE_URL" ] || return 1
    [ -n "$SOURCE_VERSION" ] || SOURCE_VERSION=$CURRENT_VERSION
    [ -n "$DOWNLOAD_REPORTED" ] || DOWNLOAD_REPORTED=0
    if [ -n "$saved_package_dir" ]; then
        OTA_PACKAGE_DIR=$saved_package_dir
        PACKAGE_STATE_FILE=$OTA_PACKAGE_DIR/.ota_remote_release.env
    fi

    if [ -z "$ARCHIVE_PATH" ]; then
        set_release_paths
    else
        PARTIAL_ARCHIVE_PATH=$ARCHIVE_PATH.part
    fi
    return 0
}

load_release_state()
{
    if load_release_state_from "$PACKAGE_STATE_FILE"; then
        return 0
    fi
    if load_release_state_from "$RELEASE_STATE_FILE"; then
        return 0
    fi

    error "release state missing"
    return 1
}

clear_release_state()
{
    rm -f "$RELEASE_STATE_FILE" "$PACKAGE_STATE_FILE"
}

read_ota_state_value()
{
    ensure_command "$OTA_INFO_BIN" "ota_info" || return 1

    ota_state_output=$("$OTA_INFO_BIN" read ota_state 2>/dev/null) || return 1
    ota_state_output=${ota_state_output#ota_state=}
    ota_state_output=${ota_state_output%% *}
    trim_value "$ota_state_output"
}

ota_state_name()
{
    case "$1" in
        0) printf 'idle\n' ;;
        6) printf 'failed\n' ;;
        11) printf 'stage1_start\n' ;;
        12) printf 'stage1_wrote\n' ;;
        13) printf 'stage1_end\n' ;;
        14) printf 'stage2_start\n' ;;
        15) printf 'stage2_wrote\n' ;;
        16) printf 'stage2_end\n' ;;
        17) printf 'complete\n' ;;
        *) printf '%s\n' "$1" ;;
    esac
}
