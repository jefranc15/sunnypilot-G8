#!/bin/bash
# Best-effort early visible banner. The real graphical screen is sunnypilot UI
# once DRM/display userspace is proven on native AGNOS.
for tty in /dev/tty0 /dev/console; do
  [ -w "$tty" ] || continue
  printf '\033[2J\033[H\n\n      AGNOS G8\n\n      LG G8 ThinQ / SM8150\n      Starting sunnypilot...\n\n' > "$tty" 2>/dev/null || true
done
exit 0
