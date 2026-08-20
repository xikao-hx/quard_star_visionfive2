#!/bin/sh

set -e

# Provider -> router -> VSHELL consumer. Do not load the direct mailbox-test
# module because it uses the same channel indexes.
echo 7 > /proc/sys/kernel/printk
insmod /mnt/starfive_ipi_mailbox.ko
insmod /mnt/quard_mbox_router.ko
insmod /mnt/quard_remote_console.ko
insmod /mnt/quard_log.ko
# insmod /mnt/quard_nor_client.ko

/mnt/soc_logd > /var/log/soc_logd.log 2>&1 &

# echo 0 > /proc/sys/kernel/printk

# test mailbox
# insmod mailbox-test.ko
# cd /sys/kernel/debug/soc:mailbox_test
# echo hello > message
# cat message
