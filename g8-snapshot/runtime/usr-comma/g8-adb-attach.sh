#!/bin/bash
set -u
LOG=/data/g8-adb-attach.log
exec >>$LOG 2>&1
UDC=a600000.dwc3
echo ===G8_ADB_ATTACH_START uptime=$(cat /proc/uptime)===
mkdir -p /config
if ! mountpoint -q /config; then
  mount -t configfs none /config || { echo CONFIGFS_MOUNT_FAILED; exit 1; }
fi
BOUND=
for i in $(seq 1 100); do
  for g in /config/usb_gadget/*; do
    [ -d $g ] || continue
    u=$(cat $g/UDC 2>/dev/null || true)
    if [ x$u = x$UDC ]; then
      BOUND=$g
      break
    fi
  done
  [ x$BOUND != x ] && break
  sleep 0.1
done
echo ===UDC===
ls -la /sys/class/udc 2>/dev/null || true
echo UDC_STATE=$(cat /sys/class/udc/$UDC/state 2>/dev/null || echo unknown)
echo ===GADGETS===
for g in /config/usb_gadget/*; do
  [ -d $g ] || continue
  echo GADGET=$g
  echo VID=$(cat $g/idVendor 2>/dev/null || echo unknown)
  echo PID=$(cat $g/idProduct 2>/dev/null || echo unknown)
  echo UDC=$(cat $g/UDC 2>/dev/null || true)
  find $g/functions -mindepth 1 -maxdepth 1 -type d -print 2>/dev/null || true
  find $g/configs -type l -print -exec readlink {} \; 2>/dev/null || true
done
if [ x$BOUND = x ]; then
  echo NO_BOUND_GADGET
  exit 2
fi
echo BOUND_GADGET=$BOUND
if [ ! -d $BOUND/functions/ffs.adb ]; then
  echo BOUND_GADGET_HAS_NO_FFS_ADB
  exit 3
fi
mkdir -p /dev/usb-ffs/adb
if ! mountpoint -q /dev/usb-ffs/adb; then
  mount -t functionfs adb /dev/usb-ffs/adb || { echo FUNCTIONFS_MOUNT_FAILED; exit 4; }
fi
echo ===FFS_BEFORE_ADBD===
ls -la /dev/usb-ffs/adb 2>/dev/null || true
echo EXEC_ADBD
exec /usr/comma/adbd
