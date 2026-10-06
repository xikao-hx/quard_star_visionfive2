command_begin()
{
    COMMAND_STAGE=$1
    need_config=$2
    need_identity=$3
    need_state=$4
    need_network=$5
    need_lock=$6

    if [ "$need_config" -ne 0 ] && ! load_client_config; then
        error "stage=$COMMAND_STAGE event=failed reason=config_load_failed config=$CLIENT_CONFIG"
        return 1
    fi
    if [ "$need_identity" -ne 0 ] && ! load_identity; then
        error "stage=$COMMAND_STAGE event=failed reason=identity_load_failed identity_config=$IDENTITY_CONFIG"
        return 1
    fi
    if [ "$need_state" -ne 0 ] && ! load_release_state; then
        error "stage=$COMMAND_STAGE event=failed reason=state_load_failed"
        return 1
    fi
    if [ "$need_network" -ne 0 ] && ! ensure_network_ready "$COMMAND_STAGE"; then
        error "stage=$COMMAND_STAGE event=failed reason=network_check_failed iface=${NETWORK_IFACE:-unknown}"
        return 1
    fi

    if [ "$need_lock" -eq 1 ]; then
        acquire_lock "${COMMAND_LOCK_NAME:-$COMMAND_STAGE}" || return 1
        COMMAND_HAS_LOCK=1
    fi

    begin_stage "$COMMAND_STAGE"
}

command_end()
{
    result=$1
    status=${2:-0}

    end_stage "$COMMAND_STAGE" "$result"
    if [ "${COMMAND_HAS_LOCK:-0}" -eq 1 ]; then
        release_lock
    fi

    COMMAND_STAGE=
    COMMAND_HAS_LOCK=0
    return "$status"
}
