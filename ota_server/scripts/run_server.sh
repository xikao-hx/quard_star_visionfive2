#!/usr/bin/env bash
set -e

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
REPO_ROOT=$(cd "$SCRIPT_DIR/../.." && pwd)

export OTA_SERVER_CONFIG="${OTA_SERVER_CONFIG:-$REPO_ROOT/ota_server/config/ota_server.conf}"

exec python3 -m ota_server.app.main
