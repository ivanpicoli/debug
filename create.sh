#!/bin/sh

insmod phantom_dev.ko
/home/ivan/lightnvm-adm/lnvm create -d phantomn0 -n phantomn_from_hell -t rrpc -o 0:0

