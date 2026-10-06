#!/bin/bash
set -e

SCRIPT_DIR=$(cd "$(dirname "$0")"; pwd)
PROJECT_ROOT=$(cd "$SCRIPT_DIR/../.."; pwd)
ROOTFS_DIR=${OTA_ROOTFS_DIR:-$PROJECT_ROOT/nfs_rootfs}
BIN_INSTALL_DIR="$ROOTFS_DIR/bin"
LIB_INSTALL_DIR="$ROOTFS_DIR/lib/ota"
LIBEXEC_INSTALL_DIR="$ROOTFS_DIR/usr/libexec/ota-client"
INIT_INSTALL_DIR="$ROOTFS_DIR/etc/init.d"
HEALTH_INSTALL_DIR="$ROOTFS_DIR/etc/ota/health-check.d"

mkdir -p "$BIN_INSTALL_DIR" "$LIB_INSTALL_DIR" "$LIBEXEC_INSTALL_DIR" \
    "$INIT_INSTALL_DIR" "$HEALTH_INSTALL_DIR"
rm -f "$BIN_INSTALL_DIR/ota_remote_client"
cp -f "$SCRIPT_DIR/ota_client.sh" "$BIN_INSTALL_DIR/ota_client"
chmod +x "$BIN_INSTALL_DIR/ota_client"
rm -f "$LIB_INSTALL_DIR/runtime.sh" "$LIB_INSTALL_DIR"/cmd_*.sh
cp -f "$SCRIPT_DIR"/lib/ota/*.sh "$LIB_INSTALL_DIR"/
chmod +x "$LIB_INSTALL_DIR"/*.sh
cp -f "$SCRIPT_DIR"/libexec/ota-client/* "$LIBEXEC_INSTALL_DIR"/
chmod +x "$LIBEXEC_INSTALL_DIR"/*
install -m 0755 \
    "$PROJECT_ROOT/target_root_script/etc/init.d/S95ota-stage2.sh" \
    "$INIT_INSTALL_DIR/S95ota-stage2.sh"

echo "Installed ota_client to $BIN_INSTALL_DIR/ota_client"
echo "Installed ota_client libs to $LIB_INSTALL_DIR"
echo "Installed ota_client commands to $LIBEXEC_INSTALL_DIR"
echo "Installed OTA recovery init script to $INIT_INSTALL_DIR/S95ota-stage2.sh"
