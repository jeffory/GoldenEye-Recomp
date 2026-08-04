#!/usr/bin/env bash
# Build the Linux amd64 `ge` binary inside the Ubuntu 24.04 release container.
#
# Building on the Fedora 44 host pins the artifacts to glibc 2.43 / GLIBCXX_3.4.35, which
# will not load on a Steam Deck (issue #12). This builds against Ubuntu 24.04's glibc 2.39 /
# libstdc++-13 instead. No source changes are involved — only the toolchain differs.
#
# Usage: scripts/build-linux-container.sh [--sdk DIR] [--rebuild-image] [-j N]
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SDK_DIR="/home/keith/Projects/GoldenEye-Recomp-rexglue"
IMAGE="goldeneye-linux-release:noble"
DOCKERFILE="$ROOT/docker/linux-release.Dockerfile"
BUILD_SUBDIR="out/build/linux-amd64-container"
REBUILD_IMAGE=0
JOBS="$(nproc)"

while [ $# -gt 0 ]; do
  case "$1" in
    --sdk) SDK_DIR="$2"; shift ;;
    --rebuild-image) REBUILD_IMAGE=1 ;;
    -j) JOBS="$2"; shift ;;
    -h|--help) sed -n '2,8p' "$0"; exit 0 ;;
    *) echo "unknown arg: $1" >&2; exit 2 ;;
  esac
  shift
done

die() { printf '\033[1;31merror:\033[0m %s\n' "$*" >&2; exit 1; }

command -v podman >/dev/null 2>&1 || die "podman not found (dnf install podman)"
[ -d "$SDK_DIR" ] || die "SDK dir not found: $SDK_DIR (pass --sdk DIR)"
[ -d "$ROOT/generated" ] || die "generated/ missing — run 'rexglue codegen' against your XEX first"
SDK_DIR="$(cd "$SDK_DIR" && pwd)"

# Container artifacts live beside, never inside, the native SDK out dir. The SDK hardcodes
# its output to ${REXGLUE_ROOT}/out/${REX_PLATFORM} (SDK CMakeLists.txt:191-193), which sits
# outside any build tree, so the only way to keep the native dev build's .so intact is to
# shadow /sdk/out with a second bind mount.
SDK_OUT="$SDK_DIR/out-container"
mkdir -p "$SDK_OUT" "$ROOT/$BUILD_SUBDIR"

if [ "$REBUILD_IMAGE" -eq 1 ] || ! podman image exists "$IMAGE"; then
  printf '\n\033[1;36m==> Building container image %s\033[0m\n' "$IMAGE"
  podman build -f "$DOCKERFILE" -t "$IMAGE" "$ROOT" || die "image build failed"
fi

printf '\n\033[1;36m==> Building ge in %s (-j %s)\033[0m\n' "$IMAGE" "$JOBS"
# --security-opt label=disable rather than :z/:Z — the latter would recursively relabel
# both source repos on SELinux, which is slow and mutates host state.
podman run --rm \
  --security-opt label=disable \
  -v "$ROOT":/work \
  -v "$SDK_DIR":/sdk \
  -v "$SDK_OUT":/sdk/out \
  -w /work \
  "$IMAGE" \
  bash -euo pipefail -c "
    cmake -S /work -B '/work/$BUILD_SUBDIR' -G Ninja \
      -DCMAKE_BUILD_TYPE=RelWithDebInfo \
      -DCMAKE_C_COMPILER=clang \
      -DCMAKE_CXX_COMPILER=clang++ \
      -DREXSDK_DIR=/sdk
    cmake --build '/work/$BUILD_SUBDIR' --target ge -j '$JOBS'
  " || die "container build failed"

GE_BIN="$(find "$ROOT/$BUILD_SUBDIR" -maxdepth 1 -type f \( -name GoldenEye -o -name ge \) | head -1)"
[ -n "$GE_BIN" ] || die "no GoldenEye/ge binary under $BUILD_SUBDIR"

printf '\n\033[1;32m==> Built\033[0m\n'
echo "GE_BIN=$GE_BIN"
echo "SDK_OUT=$SDK_OUT"
