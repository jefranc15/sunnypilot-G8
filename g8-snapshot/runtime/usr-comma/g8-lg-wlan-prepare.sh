#!/bin/bash
set -euo pipefail
MODEM=/opt/lg-android/modem
VENDOR=/opt/lg-android/vendor
SYSTEM=/opt/lg-android/system/system
PRODUCT=/opt/lg-android/product
R=/tmp/g8-wifi-android
[ -d "$MODEM/image" ] || exit 1
[ -d "$VENDOR" ] || exit 1
[ -d "$SYSTEM" ] || exit 1
bind_mount(){ src="$1"; dst="$2"; mkdir -p "$dst"; mountpoint -q "$dst" || mount --bind "$src" "$dst"; }
mkdir -p /vendor/firmware_mnt /vendor/firmware /mnt/vendor/persist /vendor/rfs /data/vendor/tombstones/rfs/modem /data/vendor/radio/modem_config
mountpoint -q /vendor/firmware_mnt || mount --bind "$MODEM" /vendor/firmware_mnt
[ ! -d "$VENDOR/firmware" ] || mountpoint -q /vendor/firmware || mount --bind "$VENDOR/firmware" /vendor/firmware
mountpoint -q /mnt/vendor/persist || mount -t ext4 /dev/block/by-name/persist /mnt/vendor/persist
mountpoint -q /vendor/rfs || mount --bind "$VENDOR/rfs" /vendor/rfs
[ ! -w /sys/module/firmware_class/parameters/path ] || printf '%s' /vendor/firmware_mnt/image > /sys/module/firmware_class/parameters/path
mkdir -p "$R"/system "$R"/vendor "$R"/product "$R"/system_ext "$R"/apex "$R"/linkerconfig "$R"/dev "$R"/proc "$R"/sys "$R"/data
bind_mount "$SYSTEM" "$R/system"
bind_mount "$VENDOR" "$R/vendor"
bind_mount "$PRODUCT" "$R/product"
[ ! -d /opt/lg-android/system/system/system_ext ] || bind_mount /opt/lg-android/system/system/system_ext "$R/system_ext"
[ ! -d /opt/lg-android/system/system/apex ] || bind_mount /opt/lg-android/system/system/apex "$R/apex"
[ ! -d /opt/lg-android/system/linkerconfig ] || bind_mount /opt/lg-android/system/linkerconfig "$R/linkerconfig"
bind_mount /dev "$R/dev"
bind_mount /proc "$R/proc"
bind_mount /sys "$R/sys"
bind_mount /data "$R/data"
mkdir -p /dev/binderfs "$R/dev/binderfs" "$R/sys/fs/selinux"
mountpoint -q /dev/binderfs || mount -t binder binder /dev/binderfs
chmod 666 /dev/binderfs/binder /dev/binderfs/hwbinder /dev/binderfs/vndbinder
rm -f /dev/hwbinder /dev/vndbinder
ln -s /dev/binderfs/hwbinder /dev/hwbinder
ln -s /dev/binderfs/vndbinder /dev/vndbinder
mountpoint -q "$R/dev/binderfs" || mount --bind /dev/binderfs "$R/dev/binderfs"
if mountpoint -q /sys/fs/selinux; then mountpoint -q "$R/sys/fs/selinux" || mount --bind /sys/fs/selinux "$R/sys/fs/selinux"; fi
if [ ! -e /data/local/tmp/libg8selinuxallow.so ] && [ -e /usr/comma/libg8selinuxallow.so ]; then cp /usr/comma/libg8selinuxallow.so /data/local/tmp/libg8selinuxallow.so; chmod 755 /data/local/tmp/libg8selinuxallow.so; fi
echo G8_WLAN_PREP=PASS
ls -l /vendor/rfs/msm/mpss/readonly/firmware/image/wlanmdsp.mbn

# G8_PERSIST_LG_BEGIN
mkdir -p /mnt/vendor/persist-lg $R/mnt/vendor/persist-lg $R/mnt/vendor/persist
mountpoint -q /mnt/vendor/persist-lg || mount -t ext4 -o ro,noload /dev/block/by-name/drm /mnt/vendor/persist-lg
mountpoint -q $R/mnt/vendor/persist-lg || mount --bind /mnt/vendor/persist-lg $R/mnt/vendor/persist-lg
mountpoint -q $R/mnt/vendor/persist || mount --bind /mnt/vendor/persist $R/mnt/vendor/persist
# G8_PERSIST_LG_END
