#!/bin/bash
set -u

PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
LOG=/var/log/g8-init.log
mkdir -p /var/log
exec >>"$LOG" 2>&1

echo "=== G8 INIT $(date -Is 2>/dev/null || true) ==="

# Never inherit Android-style shared mount propagation.
mount --make-rprivate / 2>/dev/null || true

find_part() {
  local want="$1" u base
  for u in /sys/class/block/*/uevent; do
    [ -r "$u" ] || continue
    if grep -qx "PARTNAME=$want" "$u" 2>/dev/null; then
      base="$(basename "$(dirname "$u")")"
      [ -b "/dev/$base" ] && { echo "/dev/$base"; return 0; }
    fi
  done
  return 1
}

wait_block() {
  local p="$1" i
  for i in $(seq 1 100); do
    [ -b "$p" ] && return 0
    sleep 0.05
  done
  return 1
}

mount_part_ro() {
  local name="$1" where="$2" dev
  dev="$(find_part "$name" 2>/dev/null || true)"
  [ -n "$dev" ] || { echo "partition not found: $name"; return 0; }
  mkdir -p "$where"
  mountpoint -q "$where" 2>/dev/null && return 0
  mount -o ro "$dev" "$where" || echo "WARN mount failed: $name -> $where"
}

# userdata is /dev/sda30 on the proven LM-G820UM, but resolve by GPT PARTNAME first.
DATA_DEV="$(find_part userdata 2>/dev/null || true)"
[ -n "$DATA_DEV" ] || DATA_DEV=/dev/sda30
mkdir -p /data
if ! mountpoint -q /data 2>/dev/null; then
  wait_block "$DATA_DEV" || echo "WARN userdata block device not ready: $DATA_DEV"
  mount -t ext4 -o rw,noatime,nodiratime "$DATA_DEV" /data || {
    echo "ERROR: could not mount userdata at /data"
    exit 20
  }
fi

mkdir -p \
  /data/openpilot \
  /data/tmp \
  /data/params/d \
  /data/etc \
  /data/etc/NetworkManager/system-connections \
  /data/safe_staging

chown comma:comma /data 2>/dev/null || true
chown -R comma:comma /data/tmp /data/params /data/safe_staging 2>/dev/null || true

# Preserve the proven Android-B donor runtime READ-ONLY.
mount_part_ro system_b  /opt/lg-android/system
mount_part_ro product_b /opt/lg-android/product
mount_part_ro vendor_b  /opt/lg-android/vendor
mount_part_ro modem_b   /opt/lg-android/modem

# Native AGNOS is now the kernel's host namespace, so make the full vendor
# firmware tree host-visible before KGSL/camera code can run.
for fw in \
  /opt/lg-android/vendor/firmware \
  /opt/lg-android/vendor/firmware_mnt/image
do
  if [ -d "$fw" ]; then
    if [ -w /sys/module/firmware_class/parameters/path ]; then
      printf %s "$fw" > /sys/module/firmware_class/parameters/path || true
      echo "firmware_class.path=$fw"
    fi
    break
  fi
done

# G8/SM8150 device permissions needed by the Qualcomm stack.
# G8 DRM nodes: this kernel has no devtmpfs, so create DRM char devices from sysfs.
mkdir -p /dev/dri
chmod 0755 /dev/dri 2>/dev/null || true

# DRM registration can finish slightly after early userspace starts. Wait up to
# 5 seconds for sysfs, then create the same nodes that recovery exposes.
for i in $(seq 1 50); do
  [ -r /sys/class/drm/card0/dev ] && [ -r /sys/class/drm/renderD128/dev ] && break
  sleep 0.1
done

for name in card0 renderD128; do
  sys="/sys/class/drm/$name/dev"
  node="/dev/dri/$name"
  [ -r "$sys" ] || continue

  IFS=: read -r maj min < "$sys"
  [ -n "$maj" ] && [ -n "$min" ] || continue

  if [ ! -c "$node" ]; then
    rm -f "$node"
    mknod "$node" c "$maj" "$min" || true
  fi

  chown 0:1003 "$node" 2>/dev/null || true
  chmod 0666 "$node" 2>/dev/null || true
done

# G8 ION node: this kernel has no devtmpfs, so create the misc char device from sysfs.
ion_sys=/sys/class/misc/ion/dev
ion_node=/dev/ion
if [ -r "$ion_sys" ]; then
  IFS=: read -r ion_maj ion_min < "$ion_sys"
  if [ -n "$ion_maj" ] && [ -n "$ion_min" ]; then
    if [ ! -c "$ion_node" ]; then
      rm -f "$ion_node"
      mknod "$ion_node" c "$ion_maj" "$ion_min" || true
    fi
  fi
fi

# G8 KGSL node: this kernel has no devtmpfs, so create the KGSL char device from sysfs.
kgsl_sys=/sys/class/kgsl/kgsl-3d0/dev
kgsl_node=/dev/kgsl-3d0

# KGSL registration can finish slightly after early userspace starts.
for i in $(seq 1 50); do
  [ -r "$kgsl_sys" ] && break
  sleep 0.1
done

if [ -r "$kgsl_sys" ]; then
  IFS=: read -r kgsl_maj kgsl_min < "$kgsl_sys"
  if [ -n "$kgsl_maj" ] && [ -n "$kgsl_min" ]; then
    if [ ! -c "$kgsl_node" ]; then
      rm -f "$kgsl_node"
      mknod "$kgsl_node" c "$kgsl_maj" "$kgsl_min" || true
    fi
  fi
fi

for p in /dev/kgsl-3d0 /dev/ion /dev/adsprpc-smd /dev/adsprpc-smd-secure; do
  [ -c "$p" ] || continue
  chgrp gpu "$p" 2>/dev/null || true
  chmod 0660 "$p" 2>/dev/null || true
done

for p in /dev/i2c-*; do
  [ -c "$p" ] || continue
  chgrp gpio "$p" 2>/dev/null || true
  chmod 0660 "$p" 2>/dev/null || true
done

# Camera nodes: leave ownership conservative but make them accessible to the
# comma user group for the later controlled camera bring-up.
for p in /dev/media* /dev/video* /dev/v4l-subdev*; do
  [ -c "$p" ] || continue
  chgrp video "$p" 2>/dev/null || true
  chmod 0660 "$p" 2>/dev/null || true
done

touch /run/g8-init.done
echo "G8_INIT=PASS"
