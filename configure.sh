#!/bin/sh
module="phantom_dev"
device="phantom_dev"
mode="664"

rm -f /dev/${device}0

line=$(sudo cat /proc/devices | grep phantom_dev) 
major=${line%% *}

mknod /dev/${device}0 c $major 0

chmod $mode /dev/${device}0
