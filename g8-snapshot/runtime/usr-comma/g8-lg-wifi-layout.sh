#!/bin/sh
set -eu
i=0
while [ "$i" -lt 60 ]; do
  if [ -r /opt/lg-android/vendor/etc/wifi/WCNSS_qcom_cfg.ini ] && [ -d /mnt/vendor/persist-lg/wifi/qcom ] && [ -d /vendor/firmware/wlan/qca_cld ]; then
    break
  fi
  i=$((i+1))
  sleep 1
done
test -r /opt/lg-android/vendor/etc/wifi/WCNSS_qcom_cfg.ini
test -d /mnt/vendor/persist-lg/wifi/qcom
test -d /vendor/firmware/wlan/qca_cld
mkdir -p /vendor/etc/wifi
if ! mountpoint -q /vendor/etc/wifi; then
  mount --bind /opt/lg-android/vendor/etc/wifi /vendor/etc/wifi
fi
test -r /vendor/etc/wifi/WCNSS_qcom_cfg.ini
test -r /vendor/firmware/wlan/qca_cld/WCNSS_qcom_cfg.ini
test -r /vendor/firmware/wlan/qca_cld/bdwlan.bin
test -r /vendor/firmware/wlan/qca_cld/bdwlan_ch0.bin
test -r /vendor/firmware/wlan/qca_cld/bdwlan_ch1.bin
echo G8_LG_WIFI_LAYOUT=PASS
