CONFIG_DIR=${OTA_CLIENT_ETC_DIR:-/etc/ota}
CLIENT_CONFIG=${OTA_CLIENT_CONFIG:-$CONFIG_DIR/client.conf}
EEPROM_IDENTITY_PATH=${OTA_CLIENT_EEPROM_IDENTITY_PATH:-/sys/bus/i2c/devices/0-0050/eeprom}
IDENTITY_CONFIG=${OTA_CLIENT_IDENTITY_CONFIG:-$CONFIG_DIR/device_identity.conf}
VERSION_FILE=${OTA_CLIENT_VERSION_FILE:-/etc/version}
IP_BIN=${OTA_CLIENT_IP_BIN:-ip}
DEFAULT_NETWORK_IFACE=${OTA_CLIENT_DEFAULT_NETWORK_IFACE:-eth0}
DEFAULT_HEALTH_CHECK_DIR=${OTA_CLIENT_HEALTH_CHECK_DIR:-/etc/ota/health-check.d}

ensure_command()
{
    path=$1
    name=$2
    if [ ! -x "$path" ]; then
        error "$name missing: $path"
        return 1
    fi
}

load_client_config()
{
    if [ ! -f "$CLIENT_CONFIG" ]; then
        error "config file missing: $CLIENT_CONFIG"
        return 1
    fi

    SERVER_URL=$(trim_value "$(read_kv "$CLIENT_CONFIG" "server_url")")
    MANIFEST_URL=$(trim_value "$(read_kv "$CLIENT_CONFIG" "manifest_url")")
    DOWNLOAD_DIR=$(trim_value "$(read_kv "$CLIENT_CONFIG" "download_dir")")
    LOG_PATH=$(trim_value "$(read_kv "$CLIENT_CONFIG" "log_path")")
    PUBLIC_KEY_PATH=$(trim_value "$(read_kv "$CLIENT_CONFIG" "public_key_path")")
    REQUEST_TIMEOUT=$(trim_value "$(read_kv "$CLIENT_CONFIG" "request_timeout_sec")")
    RETRY_COUNT=$(trim_value "$(read_kv "$CLIENT_CONFIG" "retry_count")")
    NETWORK_IFACE=$(trim_value "$(read_kv "$CLIENT_CONFIG" "network_iface")")
    API_TOKEN=$(trim_value "$(read_kv "$CLIENT_CONFIG" "api_token")")
    HEALTH_CHECK_DIR=$(trim_value "$(read_kv "$CLIENT_CONFIG" "health_check_dir")")
    [ -n "$HEALTH_CHECK_DIR" ] || HEALTH_CHECK_DIR=$DEFAULT_HEALTH_CHECK_DIR
    if [ -z "$SERVER_URL" ] && [ -z "$MANIFEST_URL" ]; then
        error "config invalid: missing server_url or manifest_url"
        return 1
    fi
    require_value "download_dir" "$DOWNLOAD_DIR" || return 1
    require_value "log_path" "$LOG_PATH" || return 1
    require_value "public_key_path" "$PUBLIC_KEY_PATH" || return 1
    require_value "request_timeout_sec" "$REQUEST_TIMEOUT" || return 1
    require_value "retry_count" "$RETRY_COUNT" || return 1
    require_value "network_iface" "$NETWORK_IFACE" || return 1

    if [ -n "$SERVER_URL" ]; then
        case "$SERVER_URL" in
            http://*|https://*) ;;
            *)
                error "config invalid: server_url=$SERVER_URL"
                return 1
                ;;
        esac
    fi
    if [ -n "$MANIFEST_URL" ]; then
        case "$MANIFEST_URL" in
            http://*|https://*) ;;
            *)
                error "config invalid: manifest_url=$MANIFEST_URL"
                return 1
                ;;
        esac
    fi

    if [ -z "$SERVER_URL" ] && [ -n "$API_TOKEN" ]; then
        debug "api_token ignored without server_url"
    fi

    validate_uint "$REQUEST_TIMEOUT" || {
        error "config invalid: request_timeout_sec=$REQUEST_TIMEOUT"
        return 1
    }
    validate_uint "$RETRY_COUNT" || {
        error "config invalid: retry_count=$RETRY_COUNT"
        return 1
    }
    if [ "${OTA_CLIENT_CREATE_RUNTIME_DIRS:-1}" != 0 ]; then
        mkdir -p "$DOWNLOAD_DIR" 2>/dev/null || true
    fi
    set_runtime_paths
    debug "config loaded: server_url=$SERVER_URL manifest_url=$MANIFEST_URL download_dir=$DOWNLOAD_DIR public_key_path=$PUBLIC_KEY_PATH"
}

read_eeprom_kv()
{
    key=$1

    tr '\000' '\n' < "$EEPROM_IDENTITY_PATH" | awk -F= -v wanted="$key" '
        /^[[:space:]]*#/ { next }
        $0 ~ "^[[:space:]]*" wanted "[[:space:]]*=" {
            sub(/^[^=]*=/, "", $0)
            gsub(/^[[:space:]]+|[[:space:]]+$/, "", $0)
            print
            exit
        }
    '
}

load_identity()
{
    DEVICE_ID=
    BOARD_TYPE=
    HARDWARE_VERSION=

    if [ -r "$EEPROM_IDENTITY_PATH" ]; then
        DEVICE_ID=$(trim_value "$(read_eeprom_kv "device_id")")
        BOARD_TYPE=$(trim_value "$(read_eeprom_kv "board_type")")
        HARDWARE_VERSION=$(trim_value "$(read_eeprom_kv "hardware_version")")
    else
        warn "eeprom identity missing: $EEPROM_IDENTITY_PATH"
    fi

    if [ -f "$IDENTITY_CONFIG" ]; then
        [ -n "$DEVICE_ID" ] || \
            DEVICE_ID=$(trim_value "$(read_kv "$IDENTITY_CONFIG" "device_id")")
        [ -n "$BOARD_TYPE" ] || \
            BOARD_TYPE=$(trim_value "$(read_kv "$IDENTITY_CONFIG" "board_type")")
        [ -n "$HARDWARE_VERSION" ] || \
            HARDWARE_VERSION=$(trim_value "$(read_kv "$IDENTITY_CONFIG" "hardware_version")")
    fi

    require_value "device_id" "$DEVICE_ID" || return 1
    require_value "board_type" "$BOARD_TYPE" || return 1
    require_value "hardware_version" "$HARDWARE_VERSION" || return 1

    if [ ! -f "$VERSION_FILE" ]; then
        error "version file missing: $VERSION_FILE"
        return 1
    fi
    CURRENT_VERSION=$(trim_value "$(sed -n '1p' "$VERSION_FILE")")
    require_value "current_version" "$CURRENT_VERSION" || return 1

    if [ -z "${NETWORK_IFACE:-}" ]; then
        NETWORK_IFACE=$DEFAULT_NETWORK_IFACE
    fi
    MAC_ADDR=$("$IP_BIN" link show "$NETWORK_IFACE" 2>/dev/null | awk '/link\/ether/ { print $2; exit }')
    MAC_ADDR=$(trim_value "$MAC_ADDR")
    require_value "mac" "$MAC_ADDR" || return 1
}
