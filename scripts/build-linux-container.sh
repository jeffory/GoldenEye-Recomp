#!/usr/bin/env bash
# Build the Linux amd64 `ge` binary inside the Ubuntu 24.04 release container.
#
# Building on the Fedora 44 host pins the artifacts to glibc 2.43 / GLIBCXX_3.4.35, which
# will not load on a Steam Deck (issue #12). This builds against Ubuntu 24.04's glibc 2.39 /
# libstdc++-13 instead. No source changes are involved — only the toolchain differs.
#
# Usage: scripts/build-linux-container.sh [--sdk DIR] [--rebuild-image] [--clean] [-j N]
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SDK_DIR="/home/keith/Projects/GoldenEye-Recomp-rexglue"
IMAGE="goldeneye-linux-release:noble"
DOCKERFILE="$ROOT/docker/linux-release.Dockerfile"
BUILD_SUBDIR="out/build/linux-amd64-container"
REBUILD_IMAGE=0
CLEAN=0
JOBS="$(nproc)"

while [ $# -gt 0 ]; do
  case "$1" in
    --sdk) SDK_DIR="$2"; shift ;;
    --rebuild-image) REBUILD_IMAGE=1 ;;
    --clean) CLEAN=1 ;;
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
BUILD_DIR="$ROOT/$BUILD_SUBDIR"
STAMP="$BUILD_DIR/.image-id"
mkdir -p "$SDK_OUT" "$BUILD_DIR"

if [ "$REBUILD_IMAGE" -eq 1 ] || ! podman image exists "$IMAGE"; then
  printf '\n\033[1;36m==> Building container image %s\033[0m\n' "$IMAGE"
  podman build -f "$DOCKERFILE" -t "$IMAGE" "$ROOT" || die "image build failed"
fi

IMAGE_ID="$(podman image inspect --format '{{.Id}}' "$IMAGE")" \
  || die "could not inspect image $IMAGE (was it built?)"

# A build dir's CMake cache — and clang's precompiled headers in particular — are tied to
# the exact compiler binary that produced them. The image can be rebuilt independently of
# this build dir (--rebuild-image, or just editing the Dockerfile and re-running), and
# reusing a stale cache/PCH from a different image produces confusing errors that look like
# compiler bugs rather than what they are (e.g. "PCH file built from a different branch than
# the compiler"). Wipe whenever the build dir's recorded image doesn't match the image we're
# about to use, or when there's CMake cache state with no stamp at all (builds from before
# this check existed, or a build dir touched some other way).
WIPE=0
WIPE_REASON=""
if [ "$CLEAN" -eq 1 ]; then
  WIPE=1
  WIPE_REASON="--clean requested"
elif [ -f "$STAMP" ]; then
  STAMPED_ID="$(cat "$STAMP" 2>/dev/null || true)"
  if [ "$STAMPED_ID" != "$IMAGE_ID" ]; then
    WIPE=1
    WIPE_REASON="build dir was configured against image ${STAMPED_ID:-<unreadable>}, current image is $IMAGE_ID"
  fi
elif [ -f "$BUILD_DIR/CMakeCache.txt" ]; then
  WIPE=1
  WIPE_REASON="build dir has a CMake cache but no image-id stamp (state predates this check)"
fi

if [ "$WIPE" -eq 1 ]; then
  printf '\n\033[1;33m==> Wiping stale container build state: %s\033[0m\n' "$WIPE_REASON"
  printf '    rm -rf %s %s\n' "$BUILD_DIR" "$SDK_OUT"
  rm -rf "$BUILD_DIR" "$SDK_OUT"
  mkdir -p "$BUILD_DIR" "$SDK_OUT"
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
    echo '$IMAGE_ID' > '/work/$BUILD_SUBDIR/.image-id'
    cmake --build '/work/$BUILD_SUBDIR' --target ge -j '$JOBS'
  " || die "container build failed"

GE_BIN="$(find "$ROOT/$BUILD_SUBDIR" -maxdepth 1 -type f \( -name GoldenEye -o -name ge \) | head -1)"
[ -n "$GE_BIN" ] || die "no GoldenEye/ge binary under $BUILD_SUBDIR"

printf '\n\033[1;32m==> Built\033[0m\n'
echo "GE_BIN=$GE_BIN"
echo "SDK_OUT=$SDK_OUT"
