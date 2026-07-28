#!/bin/sh
R=/opt/lg-android
exec env ANDROID_DATA=/data ANDROID_ROOT=$R/system/system LD_LIBRARY_PATH=$R/vendor/lib64:$R/system/system/apex/com.android.vndk.current/lib64:$R/system/system/apex/com.android.runtime/lib64/bionic:$R/system/system/lib64 $R/system/system/apex/com.android.runtime/bin/linker64 $R/vendor/bin/rmt_storage
