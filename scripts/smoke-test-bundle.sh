#!/usr/bin/env bash
# Prove an assembled Linux bundle loads on a stock old distro, without needing one.
#
# Runs `ldd` against the bundle inside a clean ubuntu:24.04 container carrying only *runtime*
# packages (no -dev). Anything reported "not found" is a dependency the shipped tarball
# would fail on. This verifies loading only; it does not launch the game, which needs a GPU
# and game assets.
#
# Usage: scripts/smoke-test-bundle.sh <bundle-dir>
set -euo pipefail

BUNDLE="${1:-}"
[ -n "$BUNDLE" ] || { echo "usage: $0 <bundle-dir>" >&2; exit 2; }
[ -f "$BUNDLE/ge" ] || { echo "error: no 'ge' in $BUNDLE" >&2; exit 2; }
BUNDLE="$(cd "$BUNDLE" && pwd)"

command -v podman >/dev/null 2>&1 || { echo "error: podman not found" >&2; exit 2; }

echo "==> Resolving bundle deps in a clean ubuntu:24.04 runtime"
# libgtk-3-0, libx11-xcb1, and libxi6 are all required: they are the runtime counterparts of
# the three dev packages the build image needs (gtk+-3.0, x11-xcb, xi — the SDK's pkg-config
# requirements). Installing only libgtk-3-0 leaves libX11-xcb.so.1 and libXi.so.6 absent on a
# stock ubuntu:24.04, which makes `ge`'s real libX11-xcb.so.1 dependency report "not found" and
# the smoke test fail on a perfectly good bundle. Do not trim this list back to just libgtk-3-0.
#
# libatomic1 is required too, separately from the GTK/X11 stack: `readelf -d ge` shows a direct
# NEEDED entry on libatomic.so.1 (GCC's support library for std::atomic ops the target lacks
# native instructions for). It isn't pulled in by any of the three packages above, and a stock
# ubuntu:24.04 doesn't ship it by default — confirmed by probing a real container-built bundle,
# which reported "libatomic.so.1 => not found" until this package was added.
out="$(podman run --rm --security-opt label=disable -v "$BUNDLE":/bundle:ro ubuntu:24.04 \
  bash -euo pipefail -c '
    export DEBIAN_FRONTEND=noninteractive
    apt-get update -qq >/dev/null
    apt-get install -y -qq --no-install-recommends libgtk-3-0 libx11-xcb1 libxi6 libatomic1 >/dev/null
    cd /bundle && LD_LIBRARY_PATH=/bundle ldd ./ge
  ')" || { echo "error: could not run ldd in the container" >&2; exit 2; }

echo "$out"

if printf '%s\n' "$out" | grep -q 'not found'; then
  echo >&2
  echo "FAIL: unresolved libraries on a stock ubuntu:24.04 — this bundle will not load" >&2
  printf '%s\n' "$out" | grep 'not found' >&2
  exit 1
fi

echo "SMOKE TEST OK: all libraries resolve on stock ubuntu:24.04"
