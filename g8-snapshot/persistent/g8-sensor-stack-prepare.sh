#!/bin/bash
set -euo pipefail

ROOT=/tmp/g8-wifi-android
SNS_SRC=/mnt/g8-sns-source
PERSIST_SRC=/mnt/g8-persist-source
SNS_RT=/tmp/g8-sns-cold-runtime
PERSIST_RT=/tmp/g8-persist-sensors-cold
STATE=/sys/bus/msm_subsys/devices/subsys2/state
BOOT=/sys/kernel/boot_slpi/boot
ATTEMPT=/run/g8-sensor-stack.boot-attempted
PREPARED=/run/g8-sensor-stack.prepared

echo "=== G8 SENSOR STACK PREPARE V1 ==="
echo "NO SSR. REAL SNS/PERSIST ARE READ-ONLY."

slot="$(grep -o 'androidboot.slot_suffix=[^ ]*' /proc/cmdline 2>/dev/null | head -1 | cut -d= -f2 || true)"
echo "SLOT=$slot"
[ "$slot" = "_a" ] || { echo "ERROR: not on slot _a"; exit 10; }

if [ -e "$PREPARED" ]; then
  s="$(cat "$STATE" 2>/dev/null || true)"
  echo "ALREADY_PREPARED=YES SLPI=$s"
  [ "$s" = "ONLINE" ] || {
    echo "ERROR: stack was already prepared this boot but SLPI is no longer ONLINE."
    echo "Refusing a second boot_slpi write. Reboot the device instead."
    exit 11
  }
  exit 0
fi

mount_ro_once() {
  dev="$1"
  dst="$2"
  fstype="$3"
  mkdir -p "$dst"
  if ! mountpoint -q "$dst" 2>/dev/null; then
    if [ "$fstype" = "ext4" ]; then
      mount -t ext4 -o ro,noload "$dev" "$dst"
    else
      mount -t "$fstype" -o ro "$dev" "$dst"
    fi
  fi
  opts="$(findmnt -no OPTIONS "$dst" 2>/dev/null || true)"
  case ",$opts," in
    *,ro,*|*,ro) ;;
    *) echo "ERROR: $dst is not read-only: $opts"; exit 20 ;;
  esac
}

bind_once() {
  src="$1"
  dst="$2"
  mkdir -p "$dst"
  if ! mountpoint -q "$dst" 2>/dev/null; then
    mount --bind "$src" "$dst"
  fi
}

echo "=== REAL SENSOR STORAGE ==="
mount_ro_once /dev/block/by-name/sns "$SNS_SRC" ext4
findmnt "$SNS_SRC"
test -f "$SNS_SRC/sensors/registry/registry/sns_reg_config"
test -f "$SNS_SRC/sensors/registry/registry/icm4x6xx_0.accel"
test -f "$SNS_SRC/sensors/registry/registry/icm4x6xx_0.gyro"

if mountpoint -q /mnt/vendor/persist 2>/dev/null; then
  mkdir -p /mnt/g8-persist-source
  if ! mountpoint -q /mnt/g8-persist-source 2>/dev/null; then
    mount --bind /mnt/vendor/persist /mnt/g8-persist-source
    mount -o remount,bind,ro /mnt/g8-persist-source
  fi
  findmnt /mnt/g8-persist-source
elif [ -b /dev/block/by-name/persist ]; then
  mount_ro_once /dev/block/by-name/persist /mnt/g8-persist-source ext4
  findmnt /mnt/g8-persist-source
fi

echo "=== ANDROID DONOR ROOT ==="
test -d /opt/lg-android/system/system
test -d /opt/lg-android/vendor
test -d /opt/lg-android/product

mkdir -p "$ROOT"
bind_once /opt/lg-android/system/system "$ROOT/system"
bind_once /opt/lg-android/vendor "$ROOT/vendor"
bind_once /opt/lg-android/product "$ROOT/product"

[ ! -d /opt/lg-android/system/system/system_ext ] || bind_once /opt/lg-android/system/system/system_ext "$ROOT/system_ext"
[ ! -d /opt/lg-android/vendor/odm ] || bind_once /opt/lg-android/vendor/odm "$ROOT/odm"
[ ! -d /opt/lg-android/system/system/apex ] || bind_once /opt/lg-android/system/system/apex "$ROOT/apex"
[ ! -d /opt/lg-android/system/linkerconfig ] || bind_once /opt/lg-android/system/linkerconfig "$ROOT/linkerconfig"

bind_once /dev "$ROOT/dev"
bind_once /proc "$ROOT/proc"
bind_once /sys "$ROOT/sys"
bind_once /data "$ROOT/data"

test -x "$ROOT/vendor/bin/sscrpcd"
test -x "$ROOT/vendor/bin/qrtr-lookup"

echo "=== WRITABLE SENSOR SHADOWS ==="
rm -rf "$SNS_RT" "$PERSIST_RT"
mkdir -p "$SNS_RT" "$PERSIST_RT"
cp -a "$SNS_SRC"/. "$SNS_RT"/

if [ -d "$PERSIST_SRC/sensors" ]; then
  cp -a "$PERSIST_SRC/sensors"/. "$PERSIST_RT"/
elif [ -d /mnt/vendor/persist/sensors ]; then
  cp -a /mnt/vendor/persist/sensors/. "$PERSIST_RT"/ 2>/dev/null || true
fi

mkdir -p "$SNS_RT/sensors/registry/registry"
chown 1000:1000 "$SNS_RT" "$SNS_RT/sensors" "$SNS_RT/sensors/registry" "$SNS_RT/sensors/registry/registry" 2>/dev/null || true
chmod 0755 "$SNS_RT" "$SNS_RT/sensors" "$SNS_RT/sensors/registry" "$SNS_RT/sensors/registry/registry" 2>/dev/null || true
chown -R 1000:1000 "$PERSIST_RT" 2>/dev/null || true
mkdir -p "$PERSIST_RT/registry/registry"
chown 1000:1000 "$PERSIST_RT/registry" "$PERSIST_RT/registry/registry" 2>/dev/null || true

mkdir -p /mnt/vendor/sns /mnt/vendor/persist/sensors "$ROOT/mnt/vendor/sns" "$ROOT/mnt/vendor/persist/sensors"

for dst in /mnt/vendor/sns "$ROOT/mnt/vendor/sns"; do
  if mountpoint -q "$dst" 2>/dev/null; then
    echo "EXISTING_MOUNT $dst"
  else
    mount --bind "$SNS_RT" "$dst"
  fi
done

for dst in /mnt/vendor/persist/sensors "$ROOT/mnt/vendor/persist/sensors"; do
  if mountpoint -q "$dst" 2>/dev/null; then
    echo "EXISTING_MOUNT $dst"
  else
    mount --bind "$PERSIST_RT" "$dst"
  fi
done

for f in \
  "$ROOT/mnt/vendor/sns/sensors/registry/registry/sns_reg_config" \
  "$ROOT/mnt/vendor/sns/sensors/registry/sns_reg_version" \
  "$ROOT/mnt/vendor/sns/sensors/registry/registry/icm4x6xx_0.accel" \
  "$ROOT/mnt/vendor/sns/sensors/registry/registry/icm4x6xx_0.gyro" \
  "$ROOT/vendor/etc/sensors/registry/config/sm8150_icm4x6xx_0.json" \
  "$ROOT/vendor/etc/sensors/registry/config/icm4x6xx_0.json" \
  "$ROOT/vendor/etc/sensors/registry/config/sns_gyro_cal.json"
do
  [ -f "$f" ] || { echo "ERROR: missing required sensor file $f"; exit 30; }
done

echo "=== DEVICE NODES ==="
wait_char() {
  sys="$1"
  i=0
  while [ "$i" -lt 100 ]; do
    [ -r "$sys/uevent" ] && return 0
    i=$((i+1))
    sleep 0.1
  done
  return 1
}

make_char() {
  path="$1"
  sys="$2"
  mode="$3"
  wait_char "$sys" || { echo "ERROR: kernel char registration missing $sys"; exit 40; }
  major="$(sed -n 's/^MAJOR=//p' "$sys/uevent")"
  minor="$(sed -n 's/^MINOR=//p' "$sys/uevent")"
  [ -n "$major" ] && [ -n "$minor" ] || { echo "ERROR: cannot parse $sys"; exit 41; }

  if [ ! -c "$path" ]; then
    rm -f "$path"
    mknod "$path" c "$major" "$minor"
  fi
  chown 1000:1000 "$path"
  chmod "$mode" "$path"
  ls -l "$path"
}

make_char /dev/adsprpc-smd /sys/dev/char/484:0 0664
make_char /dev/adsprpc-smd-secure /sys/dev/char/484:1 0644
make_char /dev/sensors /sys/dev/char/489:0 0660

# /dev is bind-mounted into the Android root, so these must be the same nodes.
[ -c "$ROOT/dev/adsprpc-smd" ]
[ -c "$ROOT/dev/adsprpc-smd-secure" ]
[ -c "$ROOT/dev/sensors" ]

echo "=== SLPI FIRMWARE PATH ==="
# Native firmware-mount normally provides this. Repair only when its current
# directory does not contain slpi.mdt, and use printf to avoid an embedded LF.
fw="$(cat /sys/module/firmware_class/parameters/path 2>/dev/null || true)"
if [ ! -f "$fw/slpi.mdt" ]; then
  if [ ! -f /vendor/firmware_mnt/image/slpi.mdt ] && [ -b /dev/block/by-name/modem_b ]; then
    mkdir -p /vendor/firmware_mnt
    mountpoint -q /vendor/firmware_mnt 2>/dev/null || mount -t vfat -o ro /dev/block/by-name/modem_b /vendor/firmware_mnt
  fi

  if [ -f /vendor/firmware_mnt/image/slpi.mdt ]; then
    fw=/vendor/firmware_mnt/image
  elif [ -f /opt/lg-android/vendor/firmware/slpi.mdt ]; then
    fw=/opt/lg-android/vendor/firmware
  else
    echo "ERROR: slpi.mdt not found in a proven firmware directory"
    exit 50
  fi
  printf '%s' "$fw" > /sys/module/firmware_class/parameters/path
fi
echo "FWPATH=$(cat /sys/module/firmware_class/parameters/path)"
test -f "$(cat /sys/module/firmware_class/parameters/path)/slpi.mdt"

echo "=== SLPI START ==="
state="$(cat "$STATE" 2>/dev/null || true)"
echo "SLPI_BEFORE=$state"

if [ "$state" != "ONLINE" ]; then
  case "$state" in
    OFFLINING|OFFLINE) ;;
    *)
      echo "ERROR: unexpected SLPI state '$state'; refusing boot write"
      exit 60
      ;;
  esac

  if [ -e "$ATTEMPT" ]; then
    echo "ERROR: boot_slpi was already attempted this boot; refusing a second write"
    exit 61
  fi

  # sscrpcd must not be active for this cold boot.
  if pidof sscrpcd >/dev/null 2>&1; then
    echo "ERROR: sscrpcd already running while SLPI is not ONLINE"
    exit 62
  fi

  touch "$ATTEMPT"
  echo "Issuing the one allowed normal boot_slpi write for this boot."
  echo 1 > "$BOOT"

  online=0
  for i in $(seq 1 30); do
    state="$(cat "$STATE" 2>/dev/null || true)"
    echo "SLPI_WAIT[$i]=$state"
    if [ "$state" = "ONLINE" ]; then
      online=1
      break
    fi
    sleep 1
  done
  [ "$online" -eq 1 ] || { echo "ERROR: SLPI did not reach ONLINE"; exit 63; }
fi

echo "SLPI_FINAL=$(cat "$STATE")"
touch "$PREPARED"
echo "G8_SENSOR_STACK_PREPARED=PASS"
echo "NO_SSR_WAS_REQUESTED=YES"
