#!/bin/sh

OTA_LIBEXEC_DIR=${OTA_LIBEXEC_DIR:-/usr/libexec/ota-client}

usage()
{
    cat <<'EOF'
Usage:
  ota_client identity
  ota_client config
  ota_client register
  ota_client check
  ota_client download
  ota_client unpack
  ota_client install
  ota_client service_start
  ota_client state
EOF
}

case "${1:-}" in
    identity|config|state)
        exec "$OTA_LIBEXEC_DIR/ota-device" "$@"
        ;;
    register|check|download)
        exec "$OTA_LIBEXEC_DIR/ota-remote" "$@"
        ;;
    unpack|install|service_start)
        exec "$OTA_LIBEXEC_DIR/ota-package" "$@"
        ;;
    *)
        usage
        exit 1
        ;;
esac
