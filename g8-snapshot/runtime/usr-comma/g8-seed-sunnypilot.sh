#!/bin/bash
set -euo pipefail

TAG="${G8_SUNNYPILOT_TAG:-v2026.002.002}"
DST=/data/openpilot
SEED="/data/local/tmp/sunnypilot-${TAG}-g8.tar.zst"

if [ -x "$DST/launch_g8.sh" ] && [ -f "$DST/.g8_patched" ]; then
  echo "G8 sunnypilot already installed"
  exit 0
fi

rm -rf "$DST"
mkdir -p "$DST"

if [ -f "$SEED" ]; then
  echo "Extracting pinned sunnypilot seed: $SEED"
  tar --zstd -xf "$SEED" -C "$DST"
else
  echo "Seed archive missing: $SEED"
  echo "Not cloning automatically on first boot; leaving AGNOS bootable for diagnostics."
  rm -rf "$DST"
  exit 0
fi

/usr/comma/g8-patch-sunnypilot.sh "$DST"
chown -R comma:comma "$DST"
sync
echo "G8_SUNNYPILOT_SEED=PASS"
