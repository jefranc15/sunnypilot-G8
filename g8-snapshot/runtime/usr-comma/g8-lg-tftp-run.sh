#!/bin/sh
set -eu
mkdir -p /data/vendor/pddump
chown 2903:2903 /data/vendor/pddump 2>/dev/null || true
exec env ANDROID_DATA=/data ANDROID_ROOT=/opt/lg-android/system/system LD_LIBRARY_PATH=/opt/lg-android/vendor/lib64:/opt/lg-android/system/system/apex/com.android.vndk.current/lib64:/opt/lg-android/system/system/apex/com.android.runtime/lib64/bionic:/opt/lg-android/system/system/lib64 /opt/lg-android/system/system/apex/com.android.runtime/bin/linker64 /opt/lg-android/vendor/bin/tftp_server
