#!/bin/sh

set -e

SHELL_FOLDER=$(cd "$(dirname "$0")";pwd)

cd "$SHELL_FOLDER"
echo 7 > /proc/sys/kernel/printk

if [ ! -d /sys/bus/platform/drivers/quard-ipi-mailbox ] ||
	[ ! -d /sys/bus/platform/drivers/quard-mbox-router ] ||
	[ ! -d /sys/bus/platform/drivers/quard-nor-client ]; then
	echo "Built-in IPI mailbox/router/NOR drivers are not registered" >&2
	exit 1
fi

insmod quard_remote_console.ko
insmod quard_log.ko

./soc_logd > /var/log/soc_logd.log 2>&1 &

# echo 0 > /proc/sys/kernel/printk

# test mailbox
# insmod mailbox-test.ko
# cd /sys/kernel/debug/soc:mailbox_test
# echo hello > message
# cat message
