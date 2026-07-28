#!/bin/bash
set -u

LOG=/data/g8-lg-pd-mapper.log
READY=/run/g8-lg-qrtr.ready
L=/opt/lg-android/system/system/apex/com.android.runtime/bin/linker64
BIN=/opt/lg-android/vendor/bin/pd-mapper

mkdir -p /data /run
exec >>"$LOG" 2>&1

export ANDROID_DATA=/data
export ANDROID_ROOT=/opt/lg-android/system/system
export LD_LIBRARY_PATH=/opt/lg-android/vendor/lib64:/opt/lg-android/system/system/apex/com.android.vndk.current/lib64:/opt/lg-android/system/system/apex/com.android.runtime/lib64/bionic:/opt/lg-android/system/system/lib64

echo "=== G8_LG_PD_WRAPPER_START uptime=$(cat /proc/uptime 2>/dev/null) ==="

n=0
while [ ! -e "$READY" ]; do
  n=$((n + 1))
  if [ "$n" -ge 120 ]; then
    echo "PD_WAIT_QRTR_TIMEOUT uptime=$(cat /proc/uptime 2>/dev/null)"
    exit 20
  fi
  sleep 1
done

echo "PD_QRTR_READY uptime=$(cat /proc/uptime 2>/dev/null)"
exec "$L" "$BIN"
