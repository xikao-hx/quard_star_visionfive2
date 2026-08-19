#!/bin/sh

set -eu

target_dir="$1"

mkdir -p "$target_dir/etc/network" "$target_dir/etc/init.d" \
	"$target_dir/mnt/"

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

case "$1" in
    start)
        mkdir -p "$NFS_MOUNT"
        i=0
        while [ "$i" -lt 10 ]; do
            if mountpoint -q "$NFS_MOUNT"; then
                echo "NFS root already mounted at $NFS_MOUNT"
                exit 0
            fi
            if mount -t nfs -o nolock,vers=3 "$NFS_SERVER:$NFS_EXPORT" "$NFS_MOUNT"; then
                echo "NFS root mounted at $NFS_MOUNT"
                exit 0
            fi
            i=$((i + 1))
            sleep 1
        done
        echo "WARNING: failed to mount $NFS_SERVER:$NFS_EXPORT" >&2
        ;;
    stop)
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
