#!/bin/bash
set -eu

echo "G8_DEVICE_PREPARE_V3 START"

# ---------------------------------------------------------------------------
# 1) Input nodes
# ---------------------------------------------------------------------------
mkdir -p /dev/input

touch_devnum=""
touch_real_node=""

for i in $(seq 1 100); do
  for E in /sys/class/input/event*; do
    [ -e "$E" ] || continue
    [ -r "$E/dev" ] || continue

    N="$(basename "$E")"
    D="$(cat "$E/dev" 2>/dev/null || true)"
    [ -n "$D" ] || continue

    MAJ="${D%:*}"
    MIN="${D#*:}"
    NODE="/dev/input/$N"

    if [ ! -c "$NODE" ]; then
      rm -f "$NODE"
      mknod "$NODE" c "$MAJ" "$MIN"
    fi
    chown root:input "$NODE" 2>/dev/null || true
    chmod 0660 "$NODE" 2>/dev/null || true

    NAME="$(cat "$E/device/name" 2>/dev/null || true)"
    if [ "$NAME" = "touch_dev" ]; then
      touch_devnum="$D"
      touch_real_node="$NODE"
    fi
  done

  [ -n "$touch_devnum" ] && break
  sleep 0.1
done

if [ -z "$touch_devnum" ]; then
  echo "G8_TOUCH_NODE_TIMEOUT" >&2
  exit 1
fi

# Raylib COMMA backend is hard-coded to /dev/input/event2.
# On G8, touch_dev is currently event4. Keep the real touch_dev node for
# hardwared, and make event2 an alias to the same major:minor for raylib.
TOUCH_MAJ="${touch_devnum%:*}"
TOUCH_MIN="${touch_devnum#*:}"
rm -f /dev/input/event2
mknod /dev/input/event2 c "$TOUCH_MAJ" "$TOUCH_MIN"
chown root:input /dev/input/event2
chmod 0660 /dev/input/event2

echo "G8_TOUCH_REAL=$touch_real_node:$touch_devnum"
echo "G8_TOUCH_RAYLIB_ALIAS=/dev/input/event2:$touch_devnum"

# ---------------------------------------------------------------------------
# 2) Camera/V4L2 nodes
# /dev is tmpfs on this G8 build, so kernel registration in sysfs does not
# automatically guarantee the corresponding /dev/video* or /dev/v4l-subdev*
# nodes exist.
# Gate on the nodes proven necessary for G8 camera bring-up:
#   video0 + CSIPHY-backed subdevs 9(main), 10(driver), 11(wide)
# ---------------------------------------------------------------------------
camera_ready=0

for i in $(seq 1 150); do
  for E in /sys/class/video4linux/*; do
    [ -e "$E" ] || continue
    [ -r "$E/dev" ] || continue

    N="$(basename "$E")"
    D="$(cat "$E/dev" 2>/dev/null || true)"
    [ -n "$D" ] || continue

    MAJ="${D%:*}"
    MIN="${D#*:}"
    NODE="/dev/$N"

    if [ ! -c "$NODE" ]; then
      rm -f "$NODE"
      mknod "$NODE" c "$MAJ" "$MIN"
    fi

    chown root:video "$NODE" 2>/dev/null || true
    chmod 0660 "$NODE" 2>/dev/null || true
  done

  # Some kernels expose media class nodes separately. Create them when present.
  for E in /sys/class/media/*; do
    [ -e "$E" ] || continue
    [ -r "$E/dev" ] || continue

    N="$(basename "$E")"
    D="$(cat "$E/dev" 2>/dev/null || true)"
    [ -n "$D" ] || continue

    MAJ="${D%:*}"
    MIN="${D#*:}"
    NODE="/dev/$N"

    if [ ! -c "$NODE" ]; then
      rm -f "$NODE"
      mknod "$NODE" c "$MAJ" "$MIN"
    fi

    chown root:video "$NODE" 2>/dev/null || true
    chmod 0660 "$NODE" 2>/dev/null || true
  done

  if [ -c /dev/video0 ] && [ -c /dev/v4l-subdev9 ] && [ -c /dev/v4l-subdev10 ] && [ -c /dev/v4l-subdev11 ]; then
    camera_ready=1
    break
  fi

  sleep 0.1
done

if [ "$camera_ready" -ne 1 ]; then
  echo "G8_CAMERA_NODE_TIMEOUT video0=$(test -c /dev/video0 && echo yes || echo no) subdev9=$(test -c /dev/v4l-subdev9 && echo yes || echo no) subdev10=$(test -c /dev/v4l-subdev10 && echo yes || echo no) subdev11=$(test -c /dev/v4l-subdev11 && echo yes || echo no)" >&2
  exit 1
fi

echo "G8_CAMERA_NODES_READY video0=$(stat -c '%t:%T' /dev/video0 2>/dev/null || true)"
ls -l /dev/video0 /dev/video1 /dev/video2 /dev/v4l-subdev9 /dev/v4l-subdev10 /dev/v4l-subdev11 2>/dev/null || true

# ---------------------------------------------------------------------------
# 3) Backlight permissions
# ---------------------------------------------------------------------------
for B in /sys/class/backlight/panel0-backlight /sys/class/backlight/panel0-backlight-ex; do
  [ -d "$B" ] || continue
  if [ -e "$B/bl_power" ]; then
    chgrp input "$B/bl_power" 2>/dev/null || true
    chmod 0660 "$B/bl_power" 2>/dev/null || true
  fi
  if [ -e "$B/brightness" ]; then
    chgrp input "$B/brightness" 2>/dev/null || true
    chmod 0660 "$B/brightness" 2>/dev/null || true
  fi
done

echo "G8_BACKLIGHT_PERMS_READY"
echo "G8_DEVICE_PREPARE_V3 PASS"
