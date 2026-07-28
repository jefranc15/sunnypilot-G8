#!/bin/sh
R=/opt/lg-android
i=0
while [ $i -lt 100 ]; do
  if timeout 1 env ANDROID_ROOT=$R/system/system LD_LIBRARY_PATH=$R/vendor/lib64:$R/system/system/apex/com.android.vndk.current/lib64:$R/system/system/apex/com.android.runtime/lib64/bionic:$R/system/system/lib64 $R/system/system/apex/com.android.runtime/bin/linker64 $R/vendor/bin/qrtr-lookup 14 2>/dev/null | grep -q '^[[:space:]]*14[[:space:]]'; then
    exit 0
  fi
  i=$((i+1))
  sleep 0.1
done
exit 1
