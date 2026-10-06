#!/bin/sh

set -eu

target_dir="$1"
script_dir=$(CDPATH= cd "$(dirname "$0")" && pwd)
project_dir=$(dirname "$script_dir")

install_ota_runtime()
{
	ota_info_dir="$project_dir/basic_middleware/ota_info"
	ota_package_dir="$project_dir/basic_middleware/ota_package"
	ota_client_dir="$project_dir/basic_middleware/ota_client"
	ota_conf_dir="$project_dir/target_root_script/etc/ota"
	toolchain_prefix="$project_dir/work/buildroot_initramfs/host/bin/riscv64-buildroot-linux-gnu-"
	target_sysroot="$project_dir/work/buildroot_rootfs/host/riscv64-buildroot-linux-gnu/sysroot"

	# shellcheck source=tools/script/ota_release.sh
	. "$project_dir/tools/script/ota_release.sh"
	ota_version=$(ota_read_sys_version "$ota_conf_dir/sys-version")

	for required_file in \
		"$ota_conf_dir/client.conf" \
		"$ota_conf_dir/device_identity.conf" \
		"$ota_conf_dir/keys/ota_public.pem" \
		"$target_sysroot/usr/lib/libcrypto.so.1.1"; do
		if [ ! -f "$required_file" ]; then
			printf 'Required OTA rootfs input is missing: %s\n' \
				"$required_file" >&2
			return 1
		fi
	done

	NFS_ROOT="$target_dir" CROSS_COMPILE="$toolchain_prefix" \
		"$ota_info_dir/build.sh" "variant=${OTA_BUILD_VARIANT:-userdebug}" --build
	NFS_ROOT="$target_dir" CROSS_COMPILE="$toolchain_prefix" \
		"$ota_info_dir/build.sh" "variant=${OTA_BUILD_VARIANT:-userdebug}" --install
	NFS_ROOT="$target_dir" CROSS_COMPILE="$toolchain_prefix" \
		TARGET_SYSROOT="$target_sysroot" \
		"$ota_package_dir/build.sh" "variant=${OTA_BUILD_VARIANT:-userdebug}" --build
	NFS_ROOT="$target_dir" CROSS_COMPILE="$toolchain_prefix" \
		TARGET_SYSROOT="$target_sysroot" \
		"$ota_package_dir/build.sh" "variant=${OTA_BUILD_VARIANT:-userdebug}" --install
	OTA_ROOTFS_DIR="$target_dir" "$ota_client_dir/build.sh"

	install -D -m 0755 "$project_dir/target_root_script/usr/sbin/create_link.sh" \
		"$target_dir/usr/sbin/create_link.sh"
	install -D -m 0755 "$project_dir/target_root_script/usr/sbin/ota-test.sh" \
		"$target_dir/usr/sbin/ota-test.sh"
	install -D -m 0755 "$project_dir/target_root_script/etc/init.d/S94ota-prepare.sh" \
		"$target_dir/etc/init.d/S94ota-prepare.sh"
	install -D -m 0644 "$ota_conf_dir/client.conf" \
		"$target_dir/etc/ota/client.conf"
	install -D -m 0644 "$ota_conf_dir/device_identity.conf" \
		"$target_dir/etc/ota/device_identity.conf"
	install -D -m 0644 "$ota_conf_dir/keys/ota_public.pem" \
		"$target_dir/etc/ota/keys/ota_public.pem"
	printf '%s\n' "$ota_version" > "$target_dir/etc/version"
}

mkdir -p "$target_dir/etc/network" "$target_dir/etc/init.d" \
	"$target_dir/mnt/" "$target_dir/userdata"

touch "$target_dir/etc/fstab"
if ! grep -q '^[[:space:]]*PARTLABEL=userdata[[:space:]]' \
		"$target_dir/etc/fstab"; then
	printf '%s\n' \
		'PARTLABEL=userdata  /userdata  ext4  defaults,noatime  0  2' \
		>> "$target_dir/etc/fstab"
fi

# Buildroot does not reinstall the skeleton automatically after an incremental
# edit. Keep the early init used by generated root filesystems in sync.
install -m 0755 "$project_dir/buildroot/system/skeleton/init" \
	"$target_dir/init"

cat > "$target_dir/etc/network/interfaces" <<'EOF'
# Static network for VisionFive2 AMP development.
auto lo
iface lo inet loopback

auto eth0
iface eth0 inet static
    address 192.168.5.9
    netmask 255.255.255.0
EOF

cat > "$target_dir/etc/init.d/S41nfs-root" <<'EOF'
#!/bin/sh

NFS_SERVER=192.168.5.11
NFS_EXPORT=/home/xikao/VisionFive2_6.6/nfs_rootfs
NFS_MOUNT=/mnt/
NFS_PID=/run/nfs-root.pid

mount_nfs()
{
    # Let the parent publish $! before a fast success path removes the file.
    while [ ! -s "$NFS_PID" ]; do
        sleep 0.1
    done
    trap 'rm -f "$NFS_PID"' EXIT
    i=0
    while [ "$i" -lt 30 ]; do
        if mountpoint -q "$NFS_MOUNT"; then
            echo "NFS root already mounted at $NFS_MOUNT"
            return 0
        fi
        if mount -t nfs -o nolock,vers=3,soft,timeo=10,retrans=1 \
                "$NFS_SERVER:$NFS_EXPORT" "$NFS_MOUNT"; then
            echo "NFS root mounted at $NFS_MOUNT"
            return 0
        fi
        i=$((i + 1))
        sleep 1
    done
    echo "WARNING: failed to mount $NFS_SERVER:$NFS_EXPORT" >&2
}

case "$1" in
    start)
        mkdir -p "$NFS_MOUNT"
        if mountpoint -q "$NFS_MOUNT"; then
            echo "NFS root already mounted at $NFS_MOUNT"
            exit 0
        fi
        if [ -s "$NFS_PID" ] && kill -0 "$(cat "$NFS_PID")" 2>/dev/null; then
            echo "NFS root mount is already pending"
            exit 0
        fi
        rm -f "$NFS_PID"
        echo "Starting NFS root mount in background"
        mount_nfs &
        echo $! > "$NFS_PID"
        ;;
    stop)
        if [ -s "$NFS_PID" ]; then
            kill "$(cat "$NFS_PID")" 2>/dev/null || true
            rm -f "$NFS_PID"
        fi
        umount "$NFS_MOUNT" 2>/dev/null || true
        ;;
    restart)
        "$0" stop
        "$0" start
        ;;
    *)
        echo "Usage: $0 {start|stop|restart}" >&2
        exit 1
        ;;
esac
EOF

chmod 0755 "$target_dir/etc/init.d/S41nfs-root"

if [ "${AMP_ROOTFS_INCLUDE_OTA:-1}" = 1 ]; then
	install_ota_runtime
fi
