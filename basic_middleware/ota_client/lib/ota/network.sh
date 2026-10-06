network_iface_exists()
{
    "$IP_BIN" link show "$NETWORK_IFACE" >/dev/null 2>&1
}

network_iface_has_ipv4()
{
    "$IP_BIN" -4 addr show "$NETWORK_IFACE" 2>/dev/null | grep -q 'inet '
}

ensure_network_ready()
{
    remote_stage=$1

    if ! network_iface_exists; then
        error "stage=$remote_stage event=failed reason=network_not_ready detail=iface_missing iface=$NETWORK_IFACE"
        return 1
    fi

    if ! network_iface_has_ipv4; then
        error "stage=$remote_stage event=failed reason=network_not_ready detail=no_ipv4 iface=$NETWORK_IFACE"
        return 1
    fi

    return 0
}

classify_wget_error()
{
    err_file=$1
    if [ ! -f "$err_file" ]; then
        printf 'request_failed'
        return 0
    fi

    if grep -Eiq 'bad address|Temporary failure in name resolution|Name or service not known|not known' "$err_file"; then
        printf 'dns_failed'
        return 0
    fi

    if grep -Eiq 'Network is unreachable|No route to host' "$err_file"; then
        printf 'network_unreachable'
        return 0
    fi

    if grep -Eiq 'Connection refused|Connection reset|timed out|failed: Connection refused|can.t connect' "$err_file"; then
        printf 'server_unreachable'
        return 0
    fi

    printf 'request_failed'
}

read_wget_error()
{
    err_file=$1
    if [ ! -f "$err_file" ]; then
        printf 'unknown wget error'
        return 0
    fi
    tr '\n' ' ' < "$err_file" | sed 's/[[:space:]]\+/ /g; s/[[:space:]]*$//'
}

log_wget_failure()
{
    stage_name=$1
    err_file=$2
    failure_class=$(classify_wget_error "$err_file")
    failure_detail=$(read_wget_error "$err_file")
    LAST_WGET_ERROR=$failure_detail

    case "$failure_class" in
        dns_failed)
            error "stage=$stage_name event=failed reason=dns_failed detail=$failure_detail"
            ;;
        network_unreachable)
            error "stage=$stage_name event=failed reason=network_unreachable detail=$failure_detail"
            ;;
        server_unreachable)
            error "stage=$stage_name event=failed reason=server_unreachable detail=$failure_detail"
            ;;
        *)
            error "stage=$stage_name event=failed reason=request_failed detail=$failure_detail"
            ;;
    esac
}
