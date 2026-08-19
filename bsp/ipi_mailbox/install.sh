#!/bin/sh

set -e

# Provider -> router -> VSHELL consumer. Do not load the direct mailbox-test
# module because it uses the same channel indexes.
echo 7 > /proc/sys/kernel/printk
insmod /mnt/starfive_ipi_mailbox.ko
insmod /mnt/quard_mbox_router.ko
insmod /mnt/quard_remote_console.ko
insmod /mnt/quard_nor_client.ko
# insmod quard_log.ko

# ./soc_logd > /dev/null 2>&1 &

# echo 0 > /proc/sys/kernel/printk

# test mailbox
# insmod mailbox-test.ko
# cd /sys/kernel/debug/soc:mailbox_test
# echo hello > message
# cat message
