#!/bin/sh
set -eu
i=0
while [ ! -r /sys/class/subsys/subsys_modem/dev ] && [ $i -lt 100 ]; do
  i=$((i+1))
  sleep 0.1
done
X=$(cat /sys/class/subsys/subsys_modem/dev)
MAJ=${X%:*}
MIN=${X#*:}
if [ ! -e /dev/subsys_modem ]; then
  mknod /dev/subsys_modem c $MAJ $MIN
fi
chmod 600 /dev/subsys_modem
FOUND=0
for U in /sys/class/uio/uio*; do
  [ -e "$U" ] || continue
  [ "$(cat "$U/name" 2>/dev/null)" = "rmtfs" ] || continue
  X=$(cat "$U/dev")
  MAJ=${X%:*}
  MIN=${X#*:}
  N=${U##*/}
  if [ ! -e /dev/$N ]; then
    mknod /dev/$N c $MAJ $MIN
  fi
  chmod 660 /dev/$N
  FOUND=1
  break
done
[ "$FOUND" = 1 ]
mkdir -p /dev/block/bootdevice
rm -f /dev/block/bootdevice/by-name
ln -s /dev/block/by-name /dev/block/bootdevice/by-name
mkdir -p /boot
ln -sfn /dev/block/by-name/modemst1 /boot/modem_fs1
ln -sfn /dev/block/by-name/modemst2 /boot/modem_fs2
ln -sfn /dev/block/by-name/fsc /boot/modem_fsc
ln -sfn /dev/block/by-name/fsg /boot/modem_fsg
