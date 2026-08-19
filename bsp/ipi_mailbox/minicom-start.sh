#!/bin/sh

set -e

echo 1 > /proc/sys/kernel/printk
# The remote console is already a Linux TTY.  Use a transparent byte bridge;
# minicom's curses screen loses its cursor/scroll state after one 25-line
# console page even though the underlying stream still contains valid CRLF.
exec microcom -s 115200 /dev/quard-freertos0
