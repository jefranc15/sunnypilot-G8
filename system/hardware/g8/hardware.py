from pathlib import Path
import os
import subprocess

from cereal import log
from openpilot.system.hardware.base import HardwareBase, ThermalConfig

NetworkType = log.DeviceState.NetworkType
NetworkStrength = log.DeviceState.NetworkStrength


class G8(HardwareBase):
  def get_device_type(self):
    return "tici"

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
    primary = root / "panel0-backlight"
    if primary.exists():
      return primary
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
