#!/bin/bash
set -u
LOG=/data/g8-adb-convert.log
exec >>$LOG 2>&1
G=/config/usb_gadget/g8
UDC=a600000.dwc3
echo ===G8_ADB_CONVERT_START uptime=$(cat /proc/uptime)===
mkdir -p /config
if ! mountpoint -q /config; then
  mount -t configfs none /config || { echo CONFIGFS_MOUNT_FAILED; exit 1; }
fi
for i in $(seq 1 100); do
  [ -d $G ] && break
  sleep 0.1
done
if [ ! -d $G ]; then
  echo G8_GADGET_MISSING
  exit 2
fi
echo ===BEFORE===
echo VID=$(cat $G/idVendor 2>/dev/null || echo unknown)
echo PID=$(cat $G/idProduct 2>/dev/null || echo unknown)
echo UDC=$(cat $G/UDC 2>/dev/null || true)
find $G/functions -mindepth 1 -maxdepth 1 -type d -print 2>/dev/null || true
find $G/configs -type l -print -exec readlink {} \; 2>/dev/null || true
CUR=$(cat $G/UDC 2>/dev/null || true)
if [ x$CUR != x ]; then
  echo > $G/UDC || { echo UDC_UNBIND_FAILED; exit 3; }
  echo UDC_UNBOUND
fi
sleep 0.2
find $G/configs -type l -delete 2>/dev/null || true
mkdir -p $G/configs/b.1
mkdir -p $G/functions/ffs.adb
mkdir -p /dev/usb-ffs/adb
if ! mountpoint -q /dev/usb-ffs/adb; then
  mount -t functionfs adb /dev/usb-ffs/adb || { echo FUNCTIONFS_MOUNT_FAILED; exit 4; }
fi
systemctl stop adbd.service 2>/dev/null || true
systemctl start adbd.service || { echo ADBD_START_FAILED; exit 5; }
for i in $(seq 1 50); do
  [ -e /dev/usb-ffs/adb/ep2 ] && break
  sleep 0.1
done
echo ===FFS===
ls -la /dev/usb-ffs/adb 2>/dev/null || true
if [ ! -e /dev/usb-ffs/adb/ep0 ]; then
  echo ADBD_NO_EP0
  exit 6
fi
cd $G || { echo GADGET_CD_FAILED; exit 7; }; ln -s functions/ffs.adb configs/b.1/f1 || { echo ADB_LINK_FAILED; exit 7; }
echo $UDC > $G/UDC || { echo UDC_BIND_FAILED; exit 8; }
sleep 0.5
echo ===AFTER===
echo UDC=$(cat $G/UDC 2>/dev/null || true)
echo UDC_STATE=$(cat /sys/class/udc/$UDC/state 2>/dev/null || echo unknown)
find $G/configs -type l -print -exec readlink {} \; 2>/dev/null || true
systemctl --no-pager --full status adbd.service 2>/dev/null || true
echo ===G8_ADB_CONVERT_DONE uptime=$(cat /proc/uptime)===
exit 0
