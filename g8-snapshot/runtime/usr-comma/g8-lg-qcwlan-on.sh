#!/bin/sh
set -eu
i=0
while [ ! -r /sys/class/wlan/wlan/dev ] && [ "$i" -lt 200 ]; do i=$((i+1)); sleep 0.1; done
X=$(cat /sys/class/wlan/wlan/dev)
MAJ=${X%:*}
MIN=${X#*:}
if [ ! -e /dev/wlan ]; then mknod /dev/wlan c "$MAJ" "$MIN"; fi
chmod 660 /dev/wlan
echo "qcwlanstate=$MAJ:$MIN"
if printf ON > /dev/wlan; then echo QCWLAN_ON=PASS; else echo QCWLAN_ON=TIMEOUT_OR_FAIL; fi
exit 0
