#!/bin/bash
set -u
ROOT=/tmp/g8-wifi-android

echo "=== G8 PERSISTENT BOOT VERIFY V1 ==="
echo "SLOT=$(grep -o 'androidboot.slot_suffix=[^ ]*' /proc/cmdline 2>/dev/null | head -1 | cut -d= -f2)"
echo "SLPI=$(cat /sys/bus/msm_subsys/devices/subsys2/state 2>/dev/null || true)"
echo

echo "=== UNITS ==="
for u in g8-sensor-stack.service g8-sscrpcd.service g8-openpilot-patches.service g8-openpilot.service; do
  printf '%-34s ' "$u"
  systemctl is-active "$u" 2>/dev/null || true
done

echo
echo "=== MOUNTS ==="
findmnt /mnt/g8-sns-source 2>/dev/null || true
findmnt /mnt/g8-persist-source 2>/dev/null || true
findmnt /mnt/vendor/sns 2>/dev/null || true
findmnt "$ROOT/mnt/vendor/sns" 2>/dev/null || true

echo
echo "=== NODES ==="
ls -l /dev/adsprpc-smd /dev/adsprpc-smd-secure /dev/sensors 2>/dev/null || true

echo
echo "=== SENSOR USERSPACE / SNS400 ==="
pgrep -af '[s]scrpcd' || true
if [ -x "$ROOT/vendor/bin/qrtr-lookup" ]; then
  env ANDROID_ROOT=/system ANDROID_DATA=/data \
    LD_PRELOAD=/data/local/tmp/libg8selinuxallow.so \
    LD_LIBRARY_PATH=/apex/com.android.vndk.current/lib64:/vendor/lib64:/apex/com.android.runtime/lib64/bionic:/system/lib64 \
    timeout 5 chroot "$ROOT" /vendor/bin/qrtr-lookup 400 2>&1 || true
fi

echo
echo "=== OPENPILOT PATCH MARKERS ==="
grep -nF 'os.getenv("G8_AGNOS") == "1" or os.path.isdir("/opt/lg-android/vendor")' /data/openpilot/system/sensord/sensord.py 2>/dev/null || true
grep -nF 'g8_fullbleed = os.path.isdir' /data/openpilot/selfdrive/ui/onroad/augmented_road_view.py 2>/dev/null || true
grep -nF 'LG G8 portrait-panel-in-landscape UI' /data/openpilot/selfdrive/ui/onroad/augmented_road_view.py 2>/dev/null || true

echo
echo "=== RECENT PERSISTENT LOGS ==="
tail -40 /data/g8-sensor-stack.log 2>/dev/null || true
tail -30 /data/g8-sscrpcd.log 2>/dev/null || true
tail -20 /data/g8-openpilot-patches.log 2>/dev/null || true
