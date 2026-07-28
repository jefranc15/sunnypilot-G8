#!/usr/bin/env python3
from pathlib import Path
import ast
import shutil

OP = Path("/data/openpilot")
WARN = []

def write_checked(path: Path, updated: str, backup_suffix: str):
  ast.parse(updated, filename=str(path))
  backup = path.with_name(path.name + backup_suffix)
  if not backup.exists():
    shutil.copy2(path, backup)
  path.write_text(updated, encoding="utf-8")
  ast.parse(path.read_text(encoding="utf-8"), filename=str(path))

def patch_sensord():
  p = OP / "system/sensord/sensord.py"
  if not p.is_file():
    WARN.append(f"missing {p}")
    return
  src = p.read_text(encoding="utf-8")
  new = '  if os.getenv("G8_AGNOS") == "1" or os.path.isdir("/opt/lg-android/vendor"):'
  old = '  if os.getenv("G8_AGNOS") == "1":'
  if new in src:
    print("SENSORD_G8_MARKER=ALREADY")
    return
  if src.count(old) != 1:
    WARN.append("sensord G8_AGNOS anchor not uniquely found")
    return
  updated = src.replace(old, new, 1)
  write_checked(p, updated, ".G8_PERSIST_BACKUP_20260821")
  print("SENSORD_G8_MARKER=PATCHED")

def patch_ui():
  p = OP / "selfdrive/ui/onroad/augmented_road_view.py"
  if not p.is_file():
    WARN.append(f"missing {p}")
    return
  src = p.read_text(encoding="utf-8")
  changed = False

  old_rect = """    self._content_rect = rl.Rectangle(
      rect.x + UI_BORDER_SIZE,
      rect.y + UI_BORDER_SIZE,
      rect.width - 2 * UI_BORDER_SIZE,
      rect.height - 2 * UI_BORDER_SIZE,
    )
"""
  new_rect = """    # LG G8: use the full display rect instead of comma's onroad border inset.
    g8_fullbleed = os.path.isdir("/opt/lg-android/vendor")
    border = 0 if g8_fullbleed else UI_BORDER_SIZE
    self._content_rect = rl.Rectangle(
      rect.x + border,
      rect.y + border,
      rect.width - 2 * border,
      rect.height - 2 * border,
    )
"""
  old_draw = """  def _draw_border(self, rect: rl.Rectangle):
    rl.draw_rectangle_lines_ex(rect, UI_BORDER_SIZE, rl.BLACK)
"""
  new_draw = """  def _draw_border(self, rect: rl.Rectangle):
    # LG G8 full-bleed onroad camera: no comma-style black frame.
    if os.path.isdir("/opt/lg-android/vendor"):
      return

    rl.draw_rectangle_lines_ex(rect, UI_BORDER_SIZE, rl.BLACK)
"""

  if "g8_fullbleed = os.path.isdir" not in src:
    if old_rect in src and old_draw in src:
      src = src.replace(old_rect, new_rect, 1).replace(old_draw, new_draw, 1)
      changed = True
    else:
      WARN.append("UI full-bleed anchors not found")
  else:
    print("UI_FULLBLEED=ALREADY")

  zoom_marker = "LG G8 portrait-panel-in-landscape UI"
  zoom_old = """    cx, cy = intrinsic[0, 2], intrinsic[1, 2]

    # Calculate max allowed offsets with margins
"""
  zoom_new = """    cx, cy = intrinsic[0, 2], intrinsic[1, 2]

    # LG G8 portrait-panel-in-landscape UI is wider than the comma camera viewport.
    # Use cover scaling so the road image always fills the onroad content rect.
    is_g8 = os.path.isdir("/opt/lg-android/vendor")
    if is_g8:
      zoom = max(zoom, w / (2 * cx), h / (2 * cy))

    # Calculate max allowed offsets with margins
"""
  offsets_old = """    margin = 5
    max_x_offset = cx * zoom - w / 2 - margin
    max_y_offset = cy * zoom - h / 2 - margin
"""
  offsets_new = """    margin = 5
    if is_g8:
      max_x_offset = max(0.0, cx * zoom - w / 2 - margin)
      max_y_offset = max(0.0, cy * zoom - h / 2 - margin)
    else:
      max_x_offset = cx * zoom - w / 2 - margin
      max_y_offset = cy * zoom - h / 2 - margin
"""

  if zoom_marker not in src:
    # Newer branches may already use cover scaling. Do not duplicate it.
    if "zoom = max(zoom, w / (2 * cx), h / (2 * cy))" in src:
      print("UI_ASPECT_FILL=NATIVE_OR_ALREADY")
    elif zoom_old in src and offsets_old in src:
      src = src.replace(zoom_old, zoom_new, 1).replace(offsets_old, offsets_new, 1)
      changed = True
    else:
      WARN.append("UI aspect-fill anchors not found")
  else:
    print("UI_ASPECT_FILL=ALREADY")

  if changed:
    write_checked(p, src, ".G8_PERSIST_BACKUP_20260821")
    print("UI_G8_PATCHES=PATCHED")

patch_sensord()
patch_ui()

if WARN:
  for w in WARN:
    print("WARNING:", w)

# Do not fail boot merely because a future sunnypilot source revision changed an
# anchor. Current known-good revision is patched; warnings remain visible in log.
print("G8_OPENPILOT_PATCH_GUARD_DONE=YES")
