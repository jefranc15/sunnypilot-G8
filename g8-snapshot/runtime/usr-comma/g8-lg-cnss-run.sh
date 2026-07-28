#!/bin/sh
set -eu
R=/tmp/g8-wifi-android
exec setpriv --reuid 1000 --regid 1000 --groups 1000,1010,3003,3005 --inh-caps +net_admin,+sys_chroot --ambient-caps +net_admin,+sys_chroot env ANDROID_DATA=/data ANDROID_ROOT=/system LD_LIBRARY_PATH=/apex/com.android.vndk.current/lib64:/vendor/lib64:/apex/com.android.runtime/lib64/bionic:/system/lib64 PATH=/system/bin:/vendor/bin /usr/sbin/chroot "$R" /system/bin/linker64 /vendor/bin/cnss-daemon -n -l
