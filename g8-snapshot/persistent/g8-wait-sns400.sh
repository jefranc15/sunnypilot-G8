#!/bin/bash
set -euo pipefail

ROOT=/tmp/g8-wifi-android
STATE=/sys/bus/msm_subsys/devices/subsys2/state
PRELOAD=/data/local/tmp/libg8selinuxallow.so
LD=/apex/com.android.vndk.current/lib64:/vendor/lib64:/apex/com.android.runtime/lib64/bionic:/system/lib64

echo "=== WAIT FOR LG SNS QRTR SERVICE 400 ==="

for i in $(seq 1 40); do
  state="$(cat "$STATE" 2>/dev/null || true)"
  [ "$state" = "ONLINE" ] || {
    echo "ERROR: SLPI left ONLINE while waiting for SNS400: $state"
    exit 20
  }

  if ! pidof sscrpcd >/dev/null 2>&1; then
    echo "SNS400_WAIT[$i] sscrpcd not visible yet"
    sleep 1
    continue
  fi

  out="$(
    env ANDROID_ROOT=/system ANDROID_DATA=/data \
      LD_PRELOAD="$PRELOAD" LD_LIBRARY_PATH="$LD" \
      timeout 3 chroot "$ROOT" /vendor/bin/qrtr-lookup 400 2>/dev/null || true
  )"

  if printf '%s\n' "$out" | grep -qE '^[[:space:]]*400[[:space:]]+1[[:space:]]+0[[:space:]]+'; then
    echo "$out"
    echo "SNS400_READY=PASS"
    exit 0
  fi

  echo "SNS400_WAIT[$i]"
  sleep 1
done

echo "ERROR: SNS QRTR service 400 did not appear"
exit 21
