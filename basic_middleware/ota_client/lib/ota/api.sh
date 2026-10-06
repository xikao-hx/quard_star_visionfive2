WGET_BIN=${OTA_CLIENT_WGET_BIN:-wget}

version_code()
{
    version=$1
    old_ifs=$IFS
    IFS=.
    set -- $version
    IFS=$old_ifs

    if [ $# -ne 3 ]; then
        return 1
    fi

    for item in "$1" "$2" "$3"; do
        validate_uint "$item" || return 1
        [ "$item" -le 99 ] || return 1
    done

    printf '%u\n' $((($1 * 10000) + ($2 * 100) + $3))
}

post_json()
{
    endpoint=$1
    payload=$2
    output_file=$3

    if [ -z "$SERVER_URL" ]; then
        error "request skipped: server_url is empty endpoint=$endpoint"
        return 1
    fi

    attempt=0
    max_attempts=$((RETRY_COUNT + 1))
    while [ "$attempt" -lt "$max_attempts" ]; do
        attempt=$((attempt + 1))
        err_file=$(mktemp)
        if [ -n "$API_TOKEN" ]; then
            "$WGET_BIN" -q -T "$REQUEST_TIMEOUT" \
                --header="Content-Type: application/json" \
                --header="X-OTA-Token: $API_TOKEN" \
                --post-data="$payload" \
                -O "$output_file" \
                "${SERVER_URL%/}$endpoint" 2>"$err_file" && {
                rm -f "$err_file"
                LAST_WGET_ERROR=
                return 0
            }
        else
            "$WGET_BIN" -q -T "$REQUEST_TIMEOUT" \
                --header="Content-Type: application/json" \
                --post-data="$payload" \
                -O "$output_file" \
                "${SERVER_URL%/}$endpoint" 2>"$err_file" && {
                rm -f "$err_file"
                LAST_WGET_ERROR=
                return 0
            }
        fi
        log_wget_failure "remote_api" "$err_file"
        rm -f "$err_file"
        if [ "$attempt" -lt "$max_attempts" ]; then
            info "retry $attempt/$RETRY_COUNT for $endpoint"
        fi
    done

    error "request failed: $endpoint"
    return 1
}

report_session_id()
{
    if [ -n "$SESSION_ID" ]; then
        printf '%s' "$SESSION_ID"
    elif [ -n "$RELEASE_ID" ]; then
        printf 'pre-%s' "$RELEASE_ID"
    else
        printf 'unknown-session'
    fi
}

report_stage_result()
{
    stage=$1
    result=$2
    reason_code=$3
    reason_message=$4
    report_current_version=$5
    report_target_version=$6
    session_for_report=$(report_session_id)
    response_file=$(mktemp)

    [ -n "$report_current_version" ] || report_current_version=$CURRENT_VERSION
    [ -n "$report_target_version" ] || report_target_version=$TARGET_VERSION

    if [ -z "$SERVER_URL" ]; then
        debug "report skipped: server_url is empty stage=$stage result=$result"
        return 0
    fi

    payload=$(printf '{"device_id":"%s","release_id":"%s","session_id":"%s","stage":"%s","result":"%s","current_version":"%s","target_version":"%s","reason_code":"%s","reason_message":"%s"}' \
        "$(json_escape "$DEVICE_ID")" \
        "$(json_escape "$RELEASE_ID")" \
        "$(json_escape "$session_for_report")" \
        "$(json_escape "$stage")" \
        "$(json_escape "$result")" \
        "$(json_escape "$report_current_version")" \
        "$(json_escape "$report_target_version")" \
        "$(json_escape "$reason_code")" \
        "$(json_escape "$reason_message")")

    if ! post_json "/api/v1/ota/report" "$payload" "$response_file"; then
        rm -f "$response_file"
        return 1
    fi

    if grep -q '"code"[[:space:]]*:[[:space:]]*0' "$response_file"; then
        rm -f "$response_file"
        return 0
    fi

    cat "$response_file" >&2
    rm -f "$response_file"
    return 1
}

safe_report_stage_result()
{
    if ! report_stage_result "$1" "$2" "$3" "$4" "$5" "$6"; then
        error "report failed: stage=$1 result=$2 release_id=$RELEASE_ID"
        return 1
    fi

    stage_log "$1" "report_success" "release_id=$RELEASE_ID result=$2"
    return 0
}

fetch_manifest()
{
    output_file=$1

    require_value "manifest_url" "$MANIFEST_URL" || return 1

    attempt=0
    max_attempts=$((RETRY_COUNT + 1))
    while [ "$attempt" -lt "$max_attempts" ]; do
        attempt=$((attempt + 1))
        err_file=$(mktemp)
        "$WGET_BIN" -q -T "$REQUEST_TIMEOUT" -O "$output_file" "$MANIFEST_URL" 2>"$err_file" && {
            rm -f "$err_file"
            LAST_WGET_ERROR=
            return 0
        }
        log_wget_failure "manifest" "$err_file"
        rm -f "$err_file"
        if [ "$attempt" -lt "$max_attempts" ]; then
            info "retry $attempt/$RETRY_COUNT for manifest $MANIFEST_URL"
        fi
    done

    error "manifest request failed: $MANIFEST_URL"
    return 1
}

load_release_from_manifest()
{
    response_file=$1

    if ! fetch_manifest "$response_file"; then
        error "manifest check failed: device_id=$DEVICE_ID"
        return 1
    fi

    RELEASE_ID=$(extract_json_string "release_id" "$response_file")
    RELEASE_BOARD_TYPE=$(extract_json_string "board_type" "$response_file")
    TARGET_VERSION=$(extract_json_string "sys_version" "$response_file")
    PACKAGE_URL=$(extract_json_string "package_url" "$response_file")
    PACKAGE_SHA256=$(extract_json_string "package_sha256" "$response_file")
    PACKAGE_SIZE=$(extract_json_number "package_size" "$response_file")
    SOURCE_VERSION=$CURRENT_VERSION
    SESSION_ID=
    DOWNLOAD_REPORTED=0

    require_value "release_id" "$RELEASE_ID" || return 1
    require_value "sys_version" "$TARGET_VERSION" || return 1
    require_value "package_url" "$PACKAGE_URL" || return 1
    require_value "package_sha256" "$PACKAGE_SHA256" || return 1
    if [ "$RELEASE_BOARD_TYPE" != "$BOARD_TYPE" ]; then
        error "manifest check failed: board_type mismatch local=$BOARD_TYPE remote=$RELEASE_BOARD_TYPE"
        return 1
    fi

    target_version_code=$(version_code "$TARGET_VERSION") || {
        error "manifest check failed: invalid target version=$TARGET_VERSION"
        return 1
    }
    current_version_code=$(version_code "$CURRENT_VERSION") || {
        error "manifest check failed: invalid current version=$CURRENT_VERSION"
        return 1
    }
    if [ "$target_version_code" -lt "$current_version_code" ]; then
        info "manifest check success: has_update=false device_id=$DEVICE_ID target_version=$TARGET_VERSION current_version=$CURRENT_VERSION"
        return "${FETCH_RELEASE_RC_NO_UPDATE:-20}"
    fi

    set_release_paths
    return 0
}

fetch_release_from_server()
{
    response_file=$1

    if [ -n "$MANIFEST_URL" ]; then
        load_release_from_manifest "$response_file"
        status=$?
        if [ "$status" -eq 0 ]; then
            return 0
        fi
        if [ "$status" -eq "${FETCH_RELEASE_RC_NO_UPDATE:-20}" ]; then
            return "$status"
        fi
        if [ -z "$SERVER_URL" ]; then
            return "$status"
        fi
        info "manifest check failed, fallback to server_url API"
    fi

    payload=$(printf '{"device_id":"%s","board_type":"%s","current_version":"%s"}' \
        "$(json_escape "$DEVICE_ID")" \
        "$(json_escape "$BOARD_TYPE")" \
        "$(json_escape "$CURRENT_VERSION")")

    if ! post_json "/api/v1/ota/check" "$payload" "$response_file"; then
        error "check failed: device_id=$DEVICE_ID"
        return 1
    fi

    has_update=$(extract_json_bool "has_update" "$response_file")
    if [ "$has_update" = "false" ]; then
        info "check success: has_update=false device_id=$DEVICE_ID"
        return "${FETCH_RELEASE_RC_NO_UPDATE:-20}"
    fi

    if [ "$has_update" != "true" ]; then
        error "check failed: invalid response"
        cat "$response_file" >&2
        return 1
    fi

    RELEASE_ID=$(extract_json_string "release_id" "$response_file")
    RELEASE_BOARD_TYPE=$(extract_json_string "board_type" "$response_file")
    TARGET_VERSION=$(extract_json_string "sys_version" "$response_file")
    PACKAGE_URL=$(extract_json_string "package_url" "$response_file")
    PACKAGE_SHA256=$(extract_json_string "sha256" "$response_file")
    SOURCE_VERSION=$CURRENT_VERSION
    SESSION_ID=
    DOWNLOAD_REPORTED=0

    require_value "release_id" "$RELEASE_ID" || return 1
    require_value "sys_version" "$TARGET_VERSION" || return 1
    require_value "package_url" "$PACKAGE_URL" || return 1
    require_value "sha256" "$PACKAGE_SHA256" || return 1
    if [ "$RELEASE_BOARD_TYPE" != "$BOARD_TYPE" ]; then
        error "check failed: board_type mismatch local=$BOARD_TYPE remote=$RELEASE_BOARD_TYPE"
        return 1
    fi

    set_release_paths
    return 0
}
