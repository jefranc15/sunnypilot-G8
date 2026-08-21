#!/usr/bin/env python3
"""LG G8 dummy-onroad controller for SSH use.

Bench/UI testing only. This reproduces the previous Windows CMD +
g8_dummy_onroad_v2.sh start/stop workflow directly on the device.

Usage:
  python3 /data/openpilot/scripts/dummy/g8_dummy_onroad.py start
  python3 /data/openpilot/scripts/dummy/g8_dummy_onroad.py stop
  python3 /data/openpilot/scripts/dummy/g8_dummy_onroad.py status
  python3 /data/openpilot/scripts/dummy/g8_dummy_onroad.py apply-fixes
"""

from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
import time
from pathlib import Path

OP = Path("/data/openpilot")
PUB = Path("/data/local/tmp/g8_dummy_honda_e_publishers_v3.py")
DROPIN = Path("/run/systemd/system/g8-openpilot.service.d/90-dummy-honda-e.conf")
STATE_DIR = Path("/run/g8-dummy-onroad")
ALPHA_PREV = STATE_DIR / "alpha_long_prev"
CURRENT_UNIT = "g8-dummy-onroad.service"
LEGACY_UNIT = "g8-dummy-honda-e-pub.service"
OPENPILOT_UNIT = "g8-openpilot.service"
EVENTS_PY = OP / "selfdrive/selfdrived/events.py"
REGISTRATION_PY = OP / "system/athena/registration.py"

PYTHON = "/usr/local/venv/bin/python3"
RUN_PATH = "/usr/local/venv/bin:/usr/local/bin:/usr/bin:/bin"

DROPIN_TEXT = """[Service]
Environment=NOBOARD=1
Environment=FINGERPRINT=HONDA_E
Environment=SKIP_FW_QUERY=1
Environment=G8_DUMMY_ALLOW_ONROAD=1
"""


def run(cmd: list[str], *, check: bool = True, capture: bool = False, input_text: str | None = None) -> subprocess.CompletedProcess:
  kwargs = {"check": check, "text": True}
  if capture:
    kwargs["stdout"] = subprocess.PIPE
    kwargs["stderr"] = subprocess.STDOUT
  elif input_text is not None:
    kwargs["stdout"] = subprocess.DEVNULL
  if input_text is not None:
    kwargs["input"] = input_text
  return subprocess.run(cmd, **kwargs)


def sudo(*args: str, check: bool = True, capture: bool = False, input_text: str | None = None) -> subprocess.CompletedProcess:
  return run(["sudo", *args], check=check, capture=capture, input_text=input_text)


def params():
  if str(OP) not in sys.path:
    sys.path.insert(0, str(OP))
  from openpilot.common.params import Params
  return Params()


def require_g8() -> None:
  if not Path("/G8").is_file():
    raise SystemExit("REFUSING: /G8 marker is missing; this controller is LG G8-only.")
  if not OP.is_dir():
    raise SystemExit(f"REFUSING: {OP} does not exist.")


def write_root_file(path: Path, text: str) -> None:
  sudo("mkdir", "-p", str(path.parent))
  sudo("tee", str(path), input_text=text)
  sudo("chmod", "644", str(path))


def save_alpha_state_once(p) -> None:
  if ALPHA_PREV.exists():
    return
  sudo("mkdir", "-p", str(STATE_DIR))
  raw = p.get("AlphaLongitudinalEnabled")
  previous = "MISSING" if raw is None else ("1" if p.get_bool("AlphaLongitudinalEnabled") else "0")
  sudo("tee", str(ALPHA_PREV), input_text=previous + "\n")
  sudo("chmod", "644", str(ALPHA_PREV))


def restore_alpha_state(p) -> None:
  if not ALPHA_PREV.exists():
    return
  previous = ALPHA_PREV.read_text().strip()
  if previous == "MISSING":
    p.remove("AlphaLongitudinalEnabled")
  elif previous == "1":
    p.put_bool("AlphaLongitudinalEnabled", True, block=True)
  elif previous == "0":
    p.put_bool("AlphaLongitudinalEnabled", False, block=True)
  else:
    raise RuntimeError(f"Invalid saved AlphaLongitudinalEnabled state: {previous!r}")


def stop_publishers() -> None:
  for unit in (CURRENT_UNIT, LEGACY_UNIT):
    sudo("systemctl", "stop", unit, check=False)
    sudo("systemctl", "reset-failed", unit, check=False)


def backup_once(path: Path, name: str) -> None:
  backup_root = Path("/data/g8-persistent")
  backup_root.mkdir(parents=True, exist_ok=True)
  backup = backup_root / name
  if not backup.exists():
    shutil.copy2(path, backup)
    print(f"BACKUP={backup}")


def backup_once(path: Path, name: str) -> None:
  backup_root = Path("/data/g8-persistent")
  backup_root.mkdir(parents=True, exist_ok=True)
  backup = backup_root / name
  if not backup.exists():
    shutil.copy2(path, backup)
    print(f"BACKUP={backup}")


def apply_g8_ui_fixes() -> None:
  """Suppress the G8 untested-branch startup text and comma registration alert."""
  require_g8()

  events_old = (
    'def startup_master_alert(CP: car.CarParams, CS: car.CarState, sm: messaging.SubMaster, metric: bool, soft_disable_time: int, personality) -> Alert:\n'
    '  branch = get_short_branch()  # Ensure get_short_branch is cached to avoid lags on startup\n'
    '  if "REPLAY" in os.environ:\n'
    '    branch = "replay"\n'
    '\n'
    '  return StartupAlert("WARNING: This branch is not tested", branch, alert_status=AlertStatus.userPrompt)\n'
  )
  events_new = (
    'def startup_master_alert(CP: car.CarParams, CS: car.CarState, sm: messaging.SubMaster, metric: bool, soft_disable_time: int, personality) -> Alert:\n'
    '  # LG G8 port uses a custom branch by design; keep the normal startup safety message\n'
    '  # instead of the generic untested-branch warning.\n'
    '  if os.path.isfile("/G8"):\n'
    '    return StartupAlert("Be ready to take over at any time")\n'
    '\n'
    '  branch = get_short_branch()  # Ensure get_short_branch is cached to avoid lags on startup\n'
    '  if "REPLAY" in os.environ:\n'
    '    branch = "replay"\n'
    '\n'
    '  return StartupAlert("WARNING: This branch is not tested", branch, alert_status=AlertStatus.userPrompt)\n'
  )

  events_src = EVENTS_PY.read_text()
  events_changed = False
  if "LG G8 port uses a custom branch by design" not in events_src:
    if events_old not in events_src:
      raise RuntimeError(f"Patch anchor not found in {EVENTS_PY}")
    events_src = events_src.replace(events_old, events_new, 1)
    events_changed = True

  reg_src = REGISTRATION_PY.read_text()
  reg_changed = False
  old_alert = 'set_offroad_alert("Offroad_UnregisteredHardware", (dongle_id == UNREGISTERED_DONGLE_ID) and not PC)'
  new_alert = 'set_offroad_alert("Offroad_UnregisteredHardware", (dongle_id == UNREGISTERED_DONGLE_ID) and not PC and not Path("/G8").is_file())'
  if new_alert not in reg_src:
    if old_alert not in reg_src:
      raise RuntimeError(f"Registration alert anchor not found in {REGISTRATION_PY}")
    reg_src = reg_src.replace(old_alert, new_alert, 1)
    reg_changed = True

  # Parse both candidates before changing either tracked source file.
  compile(events_src, str(EVENTS_PY), "exec")
  compile(reg_src, str(REGISTRATION_PY), "exec")

  if events_changed:
    backup_once(EVENTS_PY, "events.py.before-g8-ui-cleanup")
    EVENTS_PY.write_text(events_src)
    print(f"{EVENTS_PY}: patched")
  else:
    print(f"{EVENTS_PY}: already patched")

  if reg_changed:
    backup_once(REGISTRATION_PY, "registration.py.before-g8-ui-cleanup")
    REGISTRATION_PY.write_text(reg_src)
    print(f"{REGISTRATION_PY}: patched")
  else:
    print(f"{REGISTRATION_PY}: already patched")

  # Clear the currently stored home-screen alert. Future comma registration
  # attempts will keep it suppressed only on /G8 devices.
  params().remove("Offroad_UnregisteredHardware")

  run([sys.executable, "-m", "py_compile", str(EVENTS_PY), str(REGISTRATION_PY)])
  print("G8_UI_FIXES=PASS")
  print("Restart g8-openpilot.service when ready for the UI changes to take effect.")


def start() -> None:
  require_g8()
  if not PUB.is_file():
    raise SystemExit(f"Missing dummy publisher: {PUB}")

  p = params()
  save_alpha_state_once(p)
  p.put_bool("AlphaLongitudinalEnabled", True, block=True)

  write_root_file(DROPIN, DROPIN_TEXT)
  sudo("systemctl", "daemon-reload")

  stop_publishers()
  time.sleep(1)

  sudo(
    "systemd-run",
    "--unit=g8-dummy-onroad",
    "--collect",
    "--uid=comma",
    "--property=WorkingDirectory=/data/openpilot",
    "--setenv=HOME=/home/comma",
    "--setenv=PYTHONPATH=/data/openpilot",
    "--setenv=PYTHONUNBUFFERED=1",
    f"--setenv=PATH={RUN_PATH}",
    PYTHON,
    str(PUB),
  )

  sudo("systemctl", "restart", OPENPILOT_UNIT)
  time.sleep(5)
  print("G8_DUMMY_ONROAD=STARTED")
  status()


def stop() -> None:
  require_g8()
  p = params()

  stop_publishers()
  sudo("rm", "-f", str(DROPIN))

  restore_alpha_state(p)
  sudo("rm", "-rf", str(STATE_DIR))

  sudo("systemctl", "daemon-reload")
  sudo("systemctl", "restart", OPENPILOT_UNIT)
  time.sleep(4)
  print("G8_DUMMY_ONROAD=STOPPED")
  status()


def unit_state(unit: str) -> str:
  r = sudo("systemctl", "is-active", unit, check=False, capture=True)
  return (r.stdout or "").strip() or "unknown"


def status() -> None:
  require_g8()
  p = params()
  print(f"DROPIN={'PRESENT' if DROPIN.exists() else 'ABSENT'}")
  print(f"{CURRENT_UNIT}={unit_state(CURRENT_UNIT)}")
  print(f"{LEGACY_UNIT}={unit_state(LEGACY_UNIT)}")
  print(f"AlphaLongitudinalEnabled={p.get_bool('AlphaLongitudinalEnabled')}")
  print(f"G8_DUMMY_ALLOW_ONROAD={'1' if DROPIN.exists() else '0'}")


def main() -> None:
  parser = argparse.ArgumentParser(description="LG G8 dummy-onroad controller (bench/UI testing only)")
  parser.add_argument("action", choices=("start", "stop", "status", "apply-fixes"))
  args = parser.parse_args()

  if args.action == "start":
    start()
  elif args.action == "stop":
    stop()
  elif args.action == "apply-fixes":
    apply_g8_ui_fixes()
  else:
    status()


if __name__ == "__main__":
  main()
