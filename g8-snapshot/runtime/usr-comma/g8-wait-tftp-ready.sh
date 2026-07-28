#!/bin/sh
set -eu
i=0
while [ "$i" -lt 30 ]; do
  i=$((i+1))
  if timeout 1 env ANDROID_DATA=/data ANDROID_ROOT=/opt/lg-android/system/system LD_LIBRARY_PATH=/opt/lg-android/vendor/lib64:/opt/lg-android/system/apex/com.android.vndk.current/lib64:/opt/lg-android/system/system/apex/com.android.runtime/lib64/bionic:/opt/lg-android/system/system/lib64 /opt/lg-android/system/system/apex/com.android.runtime/bin/linker64 /opt/lg-android/vendor/bin/qrtr-lookup 4096 2>/dev/null | grep -q '^ *4096 '; then
    echo TFTP_QRTR_READY=PASS
    exit 0
  fi
  sleep 0.1
done
echo TFTP_QRTR_READY=FAIL
exit 1
