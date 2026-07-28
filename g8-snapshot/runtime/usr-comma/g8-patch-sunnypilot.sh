#!/bin/bash
set -euo pipefail
OP="${1:-/data/openpilot}"

test -d "$OP/system/hardware"
mkdir -p "$OP/system/hardware/g8"

cat > "$OP/system/hardware/g8/__init__.py" <<'PY'
PY

cat > "$OP/system/hardware/g8/hardware.py" <<'PY'
from pathlib import Path
import os
import subprocess

from cereal import log
from openpilot.system.hardware.base import HardwareBase, ThermalConfig

NetworkType = log.DeviceState.NetworkType
NetworkStrength = log.DeviceState.NetworkStrength


class G8(HardwareBase):
  def get_device_type(self):
    return "g8"

  def get_os_version(self):
    try:
      return Path("/VERSION").read_text().strip()
    except Exception:
      return "unknown"

  def get_serial(self):
    cmd = self.get_cmdline()
    return cmd.get("androidboot.serialno", "LGG8")

  def reboot(self, reason=None):
    subprocess.call(["sudo", "reboot"])

  def shutdown(self):
    subprocess.call(["sudo", "poweroff"])

  def get_network_type(self):
    try:
      with open("/proc/net/route") as f:
        for line in f.readlines()[1:]:
          p = line.split()
          if len(p) > 3 and p[1] == "00000000" and (int(p[3], 16) & 1):
            if p[0].startswith("wlan"):
              return NetworkType.wifi
            if p[0].startswith(("eth", "usb")):
              return NetworkType.ethernet
    except Exception:
      pass
    return NetworkType.none

  def get_network_strength(self, network_type):
    return NetworkStrength.unknown

  def get_thermal_config(self):
    # Safe first-boot configuration. We will map exact G8 zones after native boot.
    return ThermalConfig()

  @staticmethod
  def _backlight():
    root = Path("/sys/class/backlight")
    try:
      return next(iter(root.iterdir()))
    except Exception:
      return None

  def set_display_power(self, on: bool):
    b = self._backlight()
    if b is None:
      return
    p = b / "bl_power"
    if p.exists():
      try:
        p.write_text("0\n" if on else "4\n")
      except Exception:
        pass

  def set_screen_brightness(self, percentage):
    b = self._backlight()
    if b is None:
      return
    try:
      mx = int((b / "max_brightness").read_text())
      val = max(0, min(mx, int(mx * float(percentage) / 100.0)))
      (b / "brightness").write_text(f"{val}\n")
    except Exception:
      pass

  def get_screen_brightness(self):
    b = self._backlight()
    if b is None:
      return 0
    try:
      cur = int((b / "brightness").read_text())
      mx = max(1, int((b / "max_brightness").read_text()))
      return int(cur * 100 / mx)
    except Exception:
      return 0

  def get_voltage(self):
    for p in ("/sys/class/power_supply/battery/voltage_now",
              "/sys/class/power_supply/bms/voltage_now"):
      try:
        return int(Path(p).read_text())
      except Exception:
        pass
    return 0

  def get_current(self):
    for p in ("/sys/class/power_supply/battery/current_now",
              "/sys/class/power_supply/bms/current_now"):
      try:
        return int(Path(p).read_text())
      except Exception:
        pass
    return 0
PY

cat > "$OP/system/hardware/__init__.py" <<'PY'
import os
from typing import cast

from openpilot.system.hardware.base import HardwareBase

G8 = os.path.isfile('/G8')
TICI = os.path.isfile('/TICI')
AGNOS = os.path.isfile('/AGNOS')
PC = not TICI and not G8

if G8:
  from openpilot.system.hardware.g8.hardware import G8
  HARDWARE = cast(HardwareBase, G8())
elif TICI:
  from openpilot.system.hardware.tici.hardware import Tici
  HARDWARE = cast(HardwareBase, Tici())
else:
  from openpilot.system.hardware.pc.hardware import Pc
  HARDWARE = cast(HardwareBase, Pc())
PY

cat > "$OP/launch_g8.sh" <<'SH'
#!/usr/bin/env bash
set -euo pipefail
IFS=$'\n\t'

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$DIR"

# Keep standard sunnypilot environment, but deliberately skip Tici's
# agnos_init()/abctl/system-updater path.
source "$DIR/launch_env.sh"

ln -sfn "$DIR" /data/pythonpath
export PYTHONPATH="$DIR"
export G8_AGNOS=1

echo "=== sunnypilot G8 launcher ==="
echo "VERSION=$(cat /VERSION 2>/dev/null || true)"
echo "GIT=$(git rev-parse HEAD 2>/dev/null || true)"

cd "$DIR/system/manager"
if [ ! -f "$DIR/prebuilt" ]; then
  ./build.py
fi

exec ./manager.py
SH
chmod 0755 "$OP/launch_g8.sh"

touch "$OP/.g8_patched"
