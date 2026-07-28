#!/bin/sh
set -e
P=/sys/class/ipa/ipa/dev
for i in 1 2 3 4 5 6 7 8 9 10; do [ -r $P ] && break; sleep 1; done
[ -r $P ] || exit 20
D=$(cat $P)
MAJ=${D%:*}
MIN=${D#*:}
if [ ! -c /dev/ipa ]; then rm -f /dev/ipa; mknod /dev/ipa c $MAJ $MIN; chmod 0666 /dev/ipa; fi
echo 1 > /dev/ipa
for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15; do [ -e /sys/kernel/debug/ipa/hw_type ] && exit 0; sleep 1; done
exit 21
