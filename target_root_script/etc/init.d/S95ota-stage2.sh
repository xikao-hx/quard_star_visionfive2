#!/bin/sh

OTA_PACKAGE_SERVICE=/usr/libexec/ota-client/ota-package
OTA_CLIENT_CONFIG=${OTA_CLIENT_CONFIG:-/etc/ota/client.conf}
WAIT_OTA_NETWORK_SECS=${WAIT_OTA_NETWORK_SECS:-30}

read_config_value()
{
    key=$1
    [ -f "$OTA_CLIENT_CONFIG" ] || return 1
    sed -n "s/^[[:space:]]*$key[[:space:]]*=[[:space:]]*//p" \
        "$OTA_CLIENT_CONFIG" | sed -n '1p'
}

wait_ota_network()
{
    server_url=$(read_config_value server_url)
    [ -n "$server_url" ] || server_url=$(read_config_value manifest_url)
    network_iface=$(read_config_value network_iface)
    [ -n "$network_iface" ] || network_iface=eth0

    server_host=${server_url#*://}
    server_host=${server_host%%/*}
    server_host=${server_host%%:*}
    if [ -z "$server_host" ] || [ "$server_host" = "$server_url" ]; then
        return 0
    fi

    i=0
    while [ "$i" -lt "$WAIT_OTA_NETWORK_SECS" ]; do
        if ip addr show dev "$network_iface" 2>/dev/null | grep -q '[[:space:]]inet[[:space:]]' &&
           ip route get "$server_host" >/dev/null 2>&1; then
            return 0
        fi
        i=$((i + 1))
        sleep 1
    done

    echo "WARNING: OTA network is not ready: iface=$network_iface host=$server_host" >&2
    return 0
}

case "${1:-}" in
    start)
        if [ ! -x "$OTA_PACKAGE_SERVICE" ]; then
            echo "OTA recovery service is missing: $OTA_PACKAGE_SERVICE" >&2
            exit 1
        fi
        wait_ota_network
        echo "Starting OTA recovery service"
        "$OTA_PACKAGE_SERVICE" service_start
        rc=$?
        if [ "$rc" -ne 0 ]; then
            echo "OTA recovery service failed: rc=$rc" >&2
        fi
        exit "$rc"
        ;;
    stop)
        ;;
    restart)
        "$0" start
        ;;
    *)
        echo "Usage: $0 {start|stop|restart}" >&2
        exit 1
        ;;
esac
