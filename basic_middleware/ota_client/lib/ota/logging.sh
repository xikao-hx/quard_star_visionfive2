should_log_level()
{
    level=$1
    current_level=${OTA_CLIENT_LOG_LEVEL:-${BH_OTA_LOG_LEVEL:-INFO}}

    case "$current_level" in
        DEBUG) return 0 ;;
        INFO) [ "$level" != "DEBUG" ] ;;
        WARN) [ "$level" = "WARN" ] || [ "$level" = "ERROR" ] ;;
        ERROR) [ "$level" = "ERROR" ] ;;
        *) [ "$level" != "DEBUG" ] ;;
    esac
}

log_line()
{
    level=$1
    shift
    message=$*
    if ! should_log_level "$level"; then
        return 0
    fi
    printf '[OTA_Client][%s] %s\n' "$level" "$message"
    if [ "${OTA_CLIENT_PERSIST_LOG:-1}" != 0 ] && [ -n "${LOG_PATH:-}" ]; then
        log_dir=$(dirname "$LOG_PATH")
        mkdir -p "$log_dir" 2>/dev/null || true
        printf '[OTA_Client][%s] %s\n' "$level" "$message" >> "$LOG_PATH" 2>/dev/null || true
    fi
}

stage_boundary()
{
    info "===== stage=$1 $2 ====="
}

info()
{
    log_line INFO "$@"
}

warn()
{
    log_line WARN "$@"
}

error()
{
    log_line ERROR "$@" >&2
}

debug()
{
    log_line DEBUG "$@"
}

stage_log()
{
    stage=$1
    event=$2
    shift 2
    if [ $# -gt 0 ]; then
        info "stage=$stage event=$event $*"
    else
        info "stage=$stage event=$event"
    fi
}

now_ts()
{
    date +%s
}

record_metric()
{
    metric_line=$1
    if [ "${OTA_CLIENT_PERSIST_LOG:-1}" = 0 ] || [ -z "${METRICS_FILE:-}" ]; then
        return 0
    fi
    mkdir -p "$(dirname "$METRICS_FILE")" 2>/dev/null || true
    printf '%s\n' "$metric_line" >> "$METRICS_FILE" 2>/dev/null || true
}

begin_stage()
{
    ACTIVE_STAGE=$1
    ACTIVE_STAGE_START_TS=$(now_ts)
    ACTIVE_STAGE_LOGGED_SIZE=-1
    stage_boundary "$ACTIVE_STAGE" "started"
}

end_stage()
{
    stage_name=$1
    result=$2
    end_ts=$(now_ts)
    elapsed=0

    if [ -n "${ACTIVE_STAGE_START_TS:-}" ]; then
        elapsed=$((end_ts - ACTIVE_STAGE_START_TS))
    fi

    archive_bytes=0
    if [ -n "${ARCHIVE_PATH:-}" ] && [ -f "$ARCHIVE_PATH" ]; then
        archive_bytes=$(wc -c < "$ARCHIVE_PATH" 2>/dev/null | tr -d ' ')
        [ -n "$archive_bytes" ] || archive_bytes=0
    fi

    record_metric "ts=$end_ts stage=$stage_name result=$result elapsed_sec=$elapsed release_id=${RELEASE_ID:-unknown} session_id=$(report_session_id) archive_bytes=$archive_bytes"
    stage_boundary "$stage_name" "$result (elapsed=${elapsed}s)"
    ACTIVE_STAGE=
    ACTIVE_STAGE_START_TS=
    ACTIVE_STAGE_LOGGED_SIZE=
}
