#!/usr/bin/env python3
from pathlib import Path
import shutil

HW_PATHS = [
    Path("/data/openpilot/system/hardware/g8/hardware.py"),
    Path("/data/openpilot/openpilot/system/hardware/g8/hardware.py"),
]

UI_PATHS = [
    Path("/data/openpilot/selfdrive/ui/sunnypilot/ui_state.py"),
    Path("/data/openpilot/openpilot/selfdrive/ui/sunnypilot/ui_state.py"),
]


def unique_existing(paths):
    seen = set()
    out = []
    for p in paths:
        if not p.is_file():
            continue
        try:
            rp = str(p.resolve())
        except Exception:
            rp = str(p)
        if rp in seen:
            continue
        seen.add(rp)
        out.append(p)
    return out


def backup_once(p: Path, suffix: str):
    b = p.with_name(p.name + suffix)
    if not b.exists():
        shutil.copy2(p, b)
        print(f"BACKUP={b}")
    return b


def patch_hw(p: Path):
    s = p.read_text()

    if 'primary = root / "panel0-backlight"' in s:
        print(f"HW_ALREADY={p}")
        return

    old = '''  @staticmethod
  def _backlight():
    root = Path("/sys/class/backlight")
    try:
      return next(iter(root.iterdir()))
    except Exception:
      return None
'''

    new = '''  @staticmethod
  def _backlight():
    root = Path("/sys/class/backlight")
    primary = root / "panel0-backlight"
    if primary.exists():
      return primary
    try:
      return next(iter(root.iterdir()))
    except Exception:
      return None
'''

    if old not in s:
        raise SystemExit(f"FAIL_HW_ANCHOR:{p}")

    backup_once(p, ".pre-g8-brightness-persist-v1")
    s = s.replace(old, new, 1)
    compile(s, str(p), "exec")
    p.write_text(s)
    print(f"HW_PATCHED={p}")


def ensure_os_import(s: str, p: Path) -> str:
    if "\nimport os\n" in s or s.startswith("import os\n"):
        return s

    anchor = "from enum import Enum\n"
    if anchor in s:
        return s.replace(anchor, "import os\n" + anchor, 1)

    anchor = "import time\n"
    if anchor in s:
        return s.replace(anchor, "import os\n" + anchor, 1)

    raise SystemExit(f"FAIL_UI_IMPORT_ANCHOR:{p}")


def patch_ui(p: Path):
    s = p.read_text()

    if "# G8_DUMMY_BRIGHTNESS_V1" in s:
        print(f"UI_ALREADY={p}")
        return

    s = ensure_os_import(s, p)

    original = '''  @staticmethod
  def set_onroad_brightness(_ui_state, awake: bool, cur_brightness: float) -> float:
    if not awake or not _ui_state.started:
      return cur_brightness

    if _ui_state.onroad_brightness_timer != 0:
'''

    env_variant = '''  @staticmethod
  def set_onroad_brightness(_ui_state, awake: bool, cur_brightness: float) -> float:
    g8_dummy = os.getenv("G8_DUMMY_ALLOW_ONROAD") == "1" and not _ui_state.started
    if not awake or (not _ui_state.started and not g8_dummy):
      return cur_brightness

    if not g8_dummy and _ui_state.onroad_brightness_timer != 0:
'''

    vendor_variant = '''  @staticmethod
  def set_onroad_brightness(_ui_state, awake: bool, cur_brightness: float) -> float:
    g8_dummy = os.path.isdir("/opt/lg-android/vendor") and not _ui_state.started
    if not awake or (not _ui_state.started and not g8_dummy):
      return cur_brightness

    if not g8_dummy and _ui_state.onroad_brightness_timer != 0:
'''

    camera_variant = '''  @staticmethod
  def set_onroad_brightness(_ui_state, awake: bool, cur_brightness: float) -> float:
    g8_dummy = os.path.exists("/data/G8_CAMERA_UI_TEST") and not _ui_state.started
    if not awake or (not _ui_state.started and not g8_dummy):
      return cur_brightness

    if not g8_dummy and _ui_state.onroad_brightness_timer != 0:
'''

    current_variant = '''  @staticmethod
  def set_onroad_brightness(_ui_state, awake: bool, cur_brightness: float) -> float:
    g8_dummy = (os.path.exists("/data/G8_CAMERA_UI_TEST") or os.path.exists("/run/systemd/system/g8-openpilot.service.d/90-dummy-honda-e.conf")) and not _ui_state.started
    if not awake or (not _ui_state.started and not g8_dummy):
      return cur_brightness

    if not g8_dummy and _ui_state.onroad_brightness_timer != 0:
'''

    new = '''  @staticmethod
  def set_onroad_brightness(_ui_state, awake: bool, cur_brightness: float) -> float:
    # G8_DUMMY_BRIGHTNESS_V1
    # Dummy onroad remains logically offroad. Allow the onroad brightness
    # setting while either explicit G8 dummy marker is active.
    g8_dummy = (os.path.exists("/data/G8_CAMERA_UI_TEST") or
                os.path.exists("/run/systemd/system/g8-openpilot.service.d/90-dummy-honda-e.conf")) and not _ui_state.started

    if not awake or (not _ui_state.started and not g8_dummy):
      return cur_brightness

    # Dummy mode has no real started transition, so apply immediately.
    if not g8_dummy and _ui_state.onroad_brightness_timer != 0:
'''

    found = None
    for candidate in (current_variant, original, env_variant, vendor_variant, camera_variant):
        if candidate in s:
            found = candidate
            break

    if found is None:
        raise SystemExit(f"FAIL_UI_BRIGHTNESS_ANCHOR:{p}")

    backup_once(p, ".pre-g8-brightness-persist-v1")
    s = s.replace(found, new, 1)
    compile(s, str(p), "exec")
    p.write_text(s)
    print(f"UI_PATCHED={p}")


hw = unique_existing(HW_PATHS)
ui = unique_existing(UI_PATHS)

if not hw:
    raise SystemExit("FAIL_NO_G8_HARDWARE_FILE")
if not ui:
    raise SystemExit("FAIL_NO_UI_STATE_FILE")

for p in hw:
    patch_hw(p)

for p in ui:
    patch_ui(p)

print("G8_BRIGHTNESS_PERSIST_V1=PASS")
