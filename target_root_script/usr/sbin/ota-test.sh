#!/bin/sh

set -eu

CREATE_LINK_BIN=${CREATE_LINK_BIN:-/usr/sbin/create_link.sh}
WAIT_MTD_SECS=${WAIT_MTD_SECS:-15}

wait_for_recovery_mtd()
{
    i=0
    while [ "$i" -lt "$WAIT_MTD_SECS" ]; do
        if grep -q '"recovery"' /proc/mtd 2>/dev/null; then
            return 0
        fi
        i=$((i + 1))
        sleep 1
    done

    echo "Error: recovery MTD partition is not ready" >&2
    cat /proc/mtd >&2 2>/dev/null || true
    return 1
}

wait_for_recovery_mtd
"$CREATE_LINK_BIN"

echo "Partition symlinks updated successfully..."
