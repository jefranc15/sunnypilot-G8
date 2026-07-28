#!/bin/sh
set -eu
R=/tmp/g8-wifi-android
exec env ANDROID_DATA=/data ANDROID_ROOT=/system LD_LIBRARY_PATH=/apex/com.android.vndk.current/lib64:/vendor/lib64:/apex/com.android.runtime/lib64/bionic:/system/lib64 /usr/sbin/chroot --userspec=1000:1000 --groups=1000 "$R" /system/bin/linker64 /vendor/bin/pm-proxy
