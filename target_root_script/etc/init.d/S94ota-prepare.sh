#!/bin/sh

OTA_PREPARE_BIN=/usr/sbin/ota-test.sh

case "${1:-}" in
    start)
        if [ ! -x "$OTA_PREPARE_BIN" ]; then
            echo "OTA prepare service is missing: $OTA_PREPARE_BIN" >&2
            exit 1
        fi
        echo "Starting OTA prepare service"
        "$OTA_PREPARE_BIN"
        rc=$?
        if [ "$rc" -ne 0 ]; then
            echo "OTA prepare service failed: rc=$rc" >&2
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
