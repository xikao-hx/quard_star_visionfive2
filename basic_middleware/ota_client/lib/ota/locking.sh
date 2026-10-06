LOCK_ROOT=${OTA_CLIENT_LOCK_ROOT:-/tmp}

acquire_lock()
{
    lock_name=$1
    ACTIVE_LOCK_DIR=$LOCK_ROOT/ota_client.$lock_name.lock

    if mkdir "$ACTIVE_LOCK_DIR" 2>/dev/null; then
        printf '%s\n' "$$" > "$ACTIVE_LOCK_DIR/pid" 2>/dev/null || true
        return 0
    fi

    other_pid=
    if [ -f "$ACTIVE_LOCK_DIR/pid" ]; then
        other_pid=$(sed -n '1p' "$ACTIVE_LOCK_DIR/pid" 2>/dev/null)
    fi

    if [ -n "$other_pid" ] && kill -0 "$other_pid" 2>/dev/null; then
        warn "stage=$lock_name event=skip reason=already_running pid=$other_pid"
        return 1
    fi

    rm -rf "$ACTIVE_LOCK_DIR"
    if mkdir "$ACTIVE_LOCK_DIR" 2>/dev/null; then
        printf '%s\n' "$$" > "$ACTIVE_LOCK_DIR/pid" 2>/dev/null || true
        warn "stage=$lock_name event=lock_recovered"
        return 0
    fi

    error "stage=$lock_name event=skip reason=lock_create_failed"
    return 1
}

release_lock()
{
    if [ -n "${ACTIVE_LOCK_DIR:-}" ] && [ -d "$ACTIVE_LOCK_DIR" ]; then
        rm -rf "$ACTIVE_LOCK_DIR"
    fi
    ACTIVE_LOCK_DIR=
}
