#!/usr/bin/env bash
# Pull the device-grown shader/pipeline storage into the APK seed assets.
# Run after playing new content on the device, then commit the result.
# The seed gives first-install users a boot-time precompile instead of
# first-shot pipeline-compile hitches (see
# docs/superpowers/specs/2026-07-11-shader-seed-bundling-design.md).
#
# Usage: scripts/refresh-shader-seed.sh [adb-serial]
set -euo pipefail

DEVICE="${1:-${ANDROID_SERIAL:-}}"
ADB=(adb)
[ -n "$DEVICE" ] && ADB=(adb -s "$DEVICE")

SRC=/sdcard/Android/data/com.sunjaycy.goldeneye/files/cache/shaders/shareable
DEST="$(cd "$(dirname "$0")/.." && pwd)/android/app/src/main/assets/shader_seed"
mkdir -p "$DEST"

for f in 584108A9.xsh 584108A9.fbo.vk.xpso; do
  old=$(stat -c%s "$DEST/$f" 2>/dev/null || echo 0)
  "${ADB[@]}" pull "$SRC/$f" "$DEST/$f"
  new=$(stat -c%s "$DEST/$f")
  echo "$f: ${old} -> ${new} bytes"
done
echo "Done. Review and commit android/app/src/main/assets/shader_seed/"
