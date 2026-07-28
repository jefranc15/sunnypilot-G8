#!/usr/bin/env bash
set -euo pipefail
IFS=$'\n\t'

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$DIR"

# Keep standard sunnypilot environment, but deliberately skip Tici's
# agnos_init()/abctl/system-updater path.
export AGNOS_VERSION="${AGNOS_VERSION:-18.2}"
export PATH="/usr/local/venv/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"
export CACHEDB="/data/tinygrad-cache/cache.db"
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
