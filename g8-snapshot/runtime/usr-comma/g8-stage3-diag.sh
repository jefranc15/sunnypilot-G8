#!/bin/bash
set -u
LOG=/data/g8-stage3-diag.log
exec >>"$LOG" 2>&1

echo "=== G8_STAGE3_START uptime=$(cat /proc/uptime 2>/dev/null) ==="
mountpoint -q /sys/kernel/debug 2>/dev/null || mount -t debugfs debugfs /sys/kernel/debug 2>/dev/null || true

export ANDROID_DATA=/data
export ANDROID_ROOT=/opt/lg-android/system/system
export LD_LIBRARY_PATH=/opt/lg-android/vendor/lib64:/opt/lg-android/system/system/apex/com.android.vndk.current/lib64:/opt/lg-android/system/system/apex/com.android.runtime/lib64/bionic:/opt/lg-android/system/system/lib64
LINKER=/opt/lg-android/system/system/apex/com.android.runtime/bin/linker64
QL=/opt/lg-android/vendor/bin/qrtr-lookup

i=1
while [ "$i" -le 6 ]; do
  echo "=== SNAPSHOT $i uptime=$(cat /proc/uptime 2>/dev/null) ==="

  echo "--- QRTR ---"
  timeout 3 "$LINKER" "$QL" 2>&1 | head -150 || true

  echo "--- ICNSS ---"
  cat /sys/kernel/debug/icnss/stats 2>/dev/null | tail -80 || true

  echo "--- LINKS ---"
  ip -br link 2>/dev/null || true
  ip -br addr 2>/dev/null || true

  echo "--- DBUS ---"
  ls -l /run/dbus/system_bus_socket 2>/dev/null || echo NO_SYSTEM_DBUS
  systemctl is-active dbus.service 2>&1 || true

  echo "--- NETWORKMANAGER ---"
  systemctl is-active NetworkManager.service 2>&1 || true
  if command -v nmcli >/dev/null 2>&1; then
    nmcli -t -f GENERAL.STATE general 2>/dev/null || true
    nmcli -t -f DEVICE,TYPE,STATE dev 2>/dev/null || true
  else
    echo NMCLI_MISSING
  fi

  echo "--- SSH ---"
  systemctl is-active ssh.service 2>&1 || systemctl is-active sshd.service 2>&1 || true
  if command -v ss >/dev/null 2>&1; then
    ss -lntp 2>/dev/null | grep ':22' || true
  fi

  echo "--- DRM HOLDERS ---"
  for p in /proc/[0-9]*; do
    [ -d "$p/fd" ] || continue
    for fd in "$p"/fd/*; do
      t=$(readlink "$fd" 2>/dev/null || true)
      if [ "$t" = "/dev/dri/card0" ]; then
        pid=$(basename "$p")
        echo "PID=$pid COMM=$(cat /proc/$pid/comm 2>/dev/null)"
        tr '\000' ' ' < /proc/$pid/cmdline 2>/dev/null || true
        echo
        cat /proc/$pid/cgroup 2>/dev/null || true
      fi
    done
  done

  echo "--- DRM STATE ---"
  for c in /sys/class/drm/card0-*/status; do
    [ -f "$c" ] && echo "$c=$(cat "$c")"
  done

  echo "--- KERNEL ---"
  dmesg | grep -Ei 'icnss|wlfw|wlan_pd|servloc|qrtr|qca_cld|adreno|kgsl|drm|gbm' | tail -250 || true

  i=$((i + 1))
  sleep 10
done

echo "=== G8_STAGE3_END uptime=$(cat /proc/uptime 2>/dev/null) ==="
