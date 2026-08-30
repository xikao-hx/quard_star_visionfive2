#!/bin/sh

set -e

SHELL_FOLDER=$(cd "$(dirname "$0")";pwd)

cd "$SHELL_FOLDER"
echo 7 > /proc/sys/kernel/printk
# insmod starfive_ipi_mailbox.ko
insmod quard_mbox_router.ko
insmod quard_remote_console.ko
insmod quard_log.ko
insmod quard_nor_client.ko

./soc_logd > /var/log/soc_logd.log 2>&1 &

# echo 0 > /proc/sys/kernel/printk

# test mailbox
# insmod mailbox-test.ko
# cd /sys/kernel/debug/soc:mailbox_test
# echo hello > message
# cat message
