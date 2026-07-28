#!/bin/bash
set -u

LOG=/data/g8-lg-qrtr.log
READY=/run/g8-lg-qrtr.ready
PIDFILE=/run/g8-lg-qrtr.pid
L=/opt/lg-android/system/system/apex/com.android.runtime/bin/linker64
BIN=/opt/lg-android/vendor/bin/qrtr-ns

mkdir -p /data /run
exec >>"$LOG" 2>&1

export ANDROID_DATA=/data
export ANDROID_ROOT=/opt/lg-android/system/system
export LD_LIBRARY_PATH=/opt/lg-android/vendor/lib64:/opt/lg-android/system/system/apex/com.android.vndk.current/lib64:/opt/lg-android/system/system/apex/com.android.runtime/lib64/bionic:/opt/lg-android/system/system/lib64

child=""

cleanup() {
  rm -f "$READY" "$PIDFILE"
  if [ -n "$child" ]; then
    kill "$child" 2>/dev/null || true
    wait "$child" 2>/dev/null || true
  fi
  exit 0
}
trap cleanup TERM INT

echo "=== G8_LG_QRTR_WRAPPER_START uptime=$(cat /proc/uptime 2>/dev/null) ==="
attempt=0

while true; do
  attempt=$((attempt + 1))
  rm -f "$READY" "$PIDFILE"
  echo "QRTR_ATTEMPT=$attempt uptime=$(cat /proc/uptime 2>/dev/null)"
  "$L" "$BIN" -f &
  child=$!
  sleep 2

  if kill -0 "$child" 2>/dev/null; then
    echo "$child" > "$PIDFILE"
    touch "$READY"
    echo "QRTR_READY pid=$child uptime=$(cat /proc/uptime 2>/dev/null)"
    wait "$child"
    rc=$?
    echo "QRTR_EXIT pid=$child rc=$rc uptime=$(cat /proc/uptime 2>/dev/null)"
  else
    wait "$child"
    rc=$?
    echo "QRTR_EARLY_EXIT pid=$child rc=$rc uptime=$(cat /proc/uptime 2>/dev/null)"
  fi

  child=""
  rm -f "$READY" "$PIDFILE"
  sleep 1
done
