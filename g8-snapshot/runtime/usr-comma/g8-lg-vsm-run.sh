#!/bin/sh
set -eu
R=/tmp/g8-wifi-android
mkdir -p "$R/dev/binderfs" "$R/sys/fs/selinux"
mountpoint -q "$R/dev/binderfs" || mount --bind /dev/binderfs "$R/dev/binderfs"
if mountpoint -q /sys/fs/selinux; then mountpoint -q "$R/sys/fs/selinux" || mount --bind /sys/fs/selinux "$R/sys/fs/selinux"; fi
[ -e /dev/vndbinder ] || ln -s /dev/binderfs/vndbinder /dev/vndbinder
[ -e /dev/hwbinder ] || ln -s /dev/binderfs/hwbinder /dev/hwbinder
chmod 666 /dev/binderfs/vndbinder /dev/binderfs/hwbinder
exec env ANDROID_DATA=/data ANDROID_ROOT=/system LD_LIBRARY_PATH=/apex/com.android.vndk.current/lib64:/vendor/lib64:/apex/com.android.runtime/lib64/bionic:/system/lib64 LD_PRELOAD=/data/local/tmp/libg8selinuxallow.so /usr/sbin/chroot --userspec=1000:1000 --groups=1000,3009 "$R" /system/bin/linker64 /vendor/bin/vndservicemanager /dev/vndbinder
