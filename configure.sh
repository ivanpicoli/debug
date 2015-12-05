#!/bin/sh
module="phantom"
device="phantom"
mode="664"

rm -f /dev/${device}0

major=$(awk "\\$2==\"$module\" {print \\$1}" /proc/devices)

mknod /dev/${device}0 c $major 0

chmod $mode /dev/${device}0
