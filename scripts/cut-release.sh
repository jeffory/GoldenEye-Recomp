#!/usr/bin/env bash
# Cut a GoldenEye-Recomp release: build the signed Android APK + a Linux amd64
# bundle locally, push the version-bump commit + tag, and publish a GitHub release.
#
# Public CI cannot build the real artifacts (they embed PPC code generated from
# your own GoldenEye 007 XEX), so releases are built locally and uploaded here.
#
# Usage:
#   scripts/cut-release.sh v1.3.0-android.1 [--stable] [--allow-dirty] [--sdk DIR] [--no-container]
#
#   <version>      Required. Tag/name, e.g. v1.3.0-android.1 (leading 'v' optional
#                  in versionName, kept in the git tag).
#   --stable       Publish as a full release (default: --prerelease).
#   --allow-dirty  Skip the clean-working-tree check.
#   --sdk DIR      Path to the ReXGlue SDK checkout
#                  (default: /home/keith/Projects/GoldenEye-Recomp-rexglue).
#   --no-container Build-only mode: assemble the Linux amd64 bundle natively and stop —
#                  no version-bump commit, no Android APK, no tag/push, no GitHub release.
#                  The bundle links against THIS HOST's glibc/libstdc++ (not the release
#                  container's floor) and will NOT run on Steam Deck or older distros
#                  (issue #12); local testing only — never publish it.
set -euo pipefail

# --- args -------------------------------------------------------------------
VERSION=""
PRERELEASE=1
ALLOW_DIRTY=0
SDK_DIR="/home/keith/Projects/GoldenEye-Recomp-rexglue"
USE_CONTAINER=1
while [ $# -gt 0 ]; do
  case "$1" in
    --stable) PRERELEASE=0 ;;
    --allow-dirty) ALLOW_DIRTY=1 ;;
    --sdk) SDK_DIR="$2"; shift ;;
    --no-container) USE_CONTAINER=0 ;;
    -h|--help) sed -n '2,21p' "$0"; exit 0 ;;
    v*|[0-9]*) VERSION="$1" ;;
    *) echo "unknown arg: $1" >&2; exit 2 ;;
  esac
  shift
done
[ -n "$VERSION" ] || { echo "error: version required (e.g. v1.3.0-android.1)" >&2; exit 2; }
case "$VERSION" in v*) TAG="$VERSION" ;; *) TAG="v$VERSION" ;; esac
VNAME="${TAG#v}"   # versionName without the leading 'v'

# --- locate repo root -------------------------------------------------------
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
SDK_DIR="$(cd "$SDK_DIR" && pwd)"
GRADLE_PROPS="android/app/build.gradle"
DIST="$ROOT/dist"

step() { printf '\n\033[1;36m==> %s\033[0m\n' "$*"; }
die()  { printf '\033[1;31merror:\033[0m %s\n' "$*" >&2; exit 1; }
# Run a gate script and distinguish its two failure exit codes: 1 = a real finding (ABI
# regression / unresolved library — the thing the gate exists to catch), 2 = an infra/usage
# problem (missing objdump/podman, apt-get unreachable, bad args — see the scripts' own
# usage headers). Lumping both into one "exceeds the ABI floor" message sends you hunting a
# regression that was really just a network hiccup during the smoke test.
run_gate() {
  local desc="$1" finding_msg="$2"; shift 2
  "$@" && return 0
  local rc=$?
  # Both call sites run only after the version-bump commit above (USE_CONTAINER=1
  # is required to reach here), so on either failure path the operator is left
  # holding that local commit — tell them how to back it out.
  local recover="(a local version-bump commit exists — recover with: git reset --hard HEAD~1)"
  if [ "$rc" -eq 1 ]; then
    die "$finding_msg $recover"
  else
    die "$desc exited $rc (infra/usage error, NOT a real finding — see its output above) $recover"
  fi
}

# --- preconditions ----------------------------------------------------------
step "Preconditions"
if [ "$USE_CONTAINER" -eq 1 ]; then
  command -v gh >/dev/null   || die "gh CLI not found"
  gh auth status >/dev/null 2>&1 || die "gh not authenticated (gh auth login)"
  command -v podman >/dev/null || die "podman not found (or pass --no-container)"
fi
[ -d "$ROOT/generated" ]   || die "generated/ missing — run 'rexglue codegen' against your XEX first"
if [ "$USE_CONTAINER" -eq 1 ]; then
  [ -f "android/keystore.properties" ] || die "android/keystore.properties missing — see docs/RELEASING.md (keytool genkeypair)"
  [ -f "android/release.jks" ]         || die "android/release.jks missing — see docs/RELEASING.md"
fi
if [ "$ALLOW_DIRTY" -eq 0 ]; then
  [ -z "$(git status --porcelain)" ] || die "working tree dirty (commit/stash, or pass --allow-dirty)"
fi
git rev-parse "$TAG" >/dev/null 2>&1 && die "tag $TAG already exists"
echo "version=$VNAME tag=$TAG prerelease=$PRERELEASE sdk=$SDK_DIR use_container=$USE_CONTAINER"

# The JDK pin below and the Android build/version-bump/shader-seed steps further down all
# exist solely to support the signed-APK build and the tag/push/publish flow. None of that
# is needed for a --no-container build-only run, and requiring it would defeat the point of
# a lightweight local Linux iteration loop (see the --no-container case below).
if [ "$USE_CONTAINER" -eq 1 ]; then
  # Gradle 8.9 cannot compile build scripts under Java 23+ (the system default here
  # is Java 25). Pin JAVA_HOME to a compatible JDK (8-22) for the gradle step. Prefer
  # Gradle's own auto-provisioned Adoptium toolchains, then common system JDK paths.
  if [ -z "${JAVA_HOME:-}" ] || ! "${JAVA_HOME}/bin/javac" -version 2>&1 | grep -qE '"(1[78]|2[012])\.'; then
    GJDK=""
    for cand in \
      "$HOME"/.gradle/jdks/eclipse_adoptium-21-* \
      "$HOME"/.gradle/jdks/eclipse_adoptium-17-* \
      /usr/lib/jvm/java-21-openjdk /usr/lib/jvm/java-17-openjdk; do
      jc="$(find "$cand" -maxdepth 2 -name javac -type f 2>/dev/null | head -1)" || true
      if [ -n "$jc" ] && [ -x "$jc" ]; then GJDK="$(dirname "$(dirname "$jc")")"; break; fi
    done
    [ -n "$GJDK" ] || die "no Gradle-compatible JDK (8-22) found; default Java is too new. Install java-17/21 or let Android Studio/Gradle provision one (see docs/RELEASING.md)."
    export JAVA_HOME="$GJDK"
  fi
  echo "JAVA_HOME=$JAVA_HOME ($("$JAVA_HOME/bin/java" -version 2>&1 | head -1))"
fi

PREV_TAG="$(git describe --tags --abbrev=0 2>/dev/null || true)"

# --- version bump -----------------------------------------------------------
# Skipped entirely in build-only mode: it exists only to stamp a release, and leaving a
# stray version-bump commit behind after a local build is a trap.
if [ "$USE_CONTAINER" -eq 1 ]; then
  step "Bumping versionName -> $VNAME, versionCode++"
  CUR_CODE="$(grep -oE 'versionCode[[:space:]]+[0-9]+' "$GRADLE_PROPS" | grep -oE '[0-9]+')"
  NEW_CODE=$((CUR_CODE + 1))
  sed -i -E "s/versionCode[[:space:]]+[0-9]+/versionCode $NEW_CODE/" "$GRADLE_PROPS"
  sed -i -E "s/versionName[[:space:]]+'[^']*'/versionName '$VNAME'/" "$GRADLE_PROPS"
  grep -E 'versionCode|versionName' "$GRADLE_PROPS"
  git add "$GRADLE_PROPS"
  git commit -q -m "chore(release): $TAG"
fi

# Build BEFORE pushing/tagging so a build failure never leaves a dangling tag.
# The version-bump commit above stays local until the builds succeed; if a build
# fails, reset it with: git reset --hard HEAD~1

# --- shader seed freshness (warn only) --------------------------------------
# The bundled first-install shader seed should track real playthrough coverage;
# refresh with scripts/refresh-shader-seed.sh after playing new content. Android-only
# concern, so it's skipped along with the APK build in --no-container mode.
if [ "$USE_CONTAINER" -eq 1 ]; then
  SEED_DIR="android/app/src/main/assets/shader_seed"
  if [ -d "$SEED_DIR" ]; then
    SEED_COMMIT_TS=$(git log -1 --format=%ct -- "$SEED_DIR" 2>/dev/null || echo "")
    if [ -n "$SEED_COMMIT_TS" ]; then
      SEED_AGE_DAYS=$(( ( $(date +%s) - SEED_COMMIT_TS ) / 86400 ))
      if [ "$SEED_AGE_DAYS" -gt 30 ]; then
        echo "WARNING: shader seed is ${SEED_AGE_DAYS} days old -- consider scripts/refresh-shader-seed.sh" >&2
      fi
    fi
  fi
fi

# --- prepare dist dir ---------------------------------------------------------
if [ "$USE_CONTAINER" -eq 1 ]; then
  # Full release: wipe dist/ so no stale artifact from a previous run can leak into
  # this release's upload.
  rm -rf "$DIST"; mkdir -p "$DIST"
else
  # Build-only mode: dist/ may hold a previous real release's APK/tarball/notes —
  # do not touch those. Only clear the bundle subdirectory this run is about to
  # rebuild.
  mkdir -p "$DIST"
  rm -rf "$DIST/bundle"
fi

# --- build: Android signed release APK -------------------------------------
# Skipped in build-only mode: it serves no purpose in a Linux-only local build and needs
# the signing keystore, which build-only runs are not required to have.
if [ "$USE_CONTAINER" -eq 1 ]; then
  APK_OUT="$DIST/GoldenEye-Recomp-$TAG-android-arm64.apk"
  step "Building signed Android release APK"
  ( cd android && ./gradlew :app:assembleRelease -PrexSdkDir="$SDK_DIR" )
  SIGNED_APK="android/app/build/outputs/apk/release/app-release.apk"
  [ -f "$SIGNED_APK" ] || die "expected signed APK at $SIGNED_APK (unsigned build? check keystore.properties)"
  cp "$SIGNED_APK" "$APK_OUT"
fi

# --- build: Linux amd64 release bundle -------------------------------------
# Built in the Ubuntu 24.04 container by default: a native Fedora build inherits this
# host's glibc 2.43 / GLIBCXX_3.4.35 floor and will not load on a Steam Deck (#12).
if [ "$USE_CONTAINER" -eq 1 ]; then
  step "Building Linux amd64 in the release container (glibc 2.39 floor)"
  "$ROOT/scripts/build-linux-container.sh" --sdk "$SDK_DIR"
  GE_BIN="$(find out/build/linux-amd64-container -maxdepth 1 -type f \( -name GoldenEye -o -name ge \) | head -1)"
  SDK_LIB_DIR="$SDK_DIR/out-container/linux-amd64"
else
  step "Building Linux amd64 NATIVELY (relwithdebinfo) — build-only, NOT portable to older distros"
  cmake --build --preset linux-amd64-relwithdebinfo --target ge
  # The CMake target is `ge` but its OUTPUT_NAME is `GoldenEye`, so the built file
  # is `GoldenEye` (older builds emitted `ge`). Accept either.
  GE_BIN="$(find out/build/linux-amd64-relwithdebinfo -maxdepth 1 -type f \( -name GoldenEye -o -name ge \) | head -1)"
  SDK_LIB_DIR="$SDK_DIR/out/linux-amd64"
fi
[ -n "$GE_BIN" ] && [ -f "$GE_BIN" ] || die "GoldenEye/ge binary not found"

step "Assembling Linux bundle (ge + resolved .so deps)"
BUNDLE="$DIST/bundle"
mkdir -p "$BUNDLE"
cp "$GE_BIN" "$BUNDLE/ge"
# Copy every dependency ldd resolves out of the SDK out dir (rd/non-rd agnostic). Under
# `pipefail`, grep matching nothing would otherwise kill the script silently (no message)
# right after a full build — capture the list first and fail loudly, naming the directory,
# if it comes back empty. This is the exact failure mode the linux-amd64 subdir fix exists
# to prevent, so it should explain itself if it ever regresses.
SDK_SO_LIST="$(LD_LIBRARY_PATH="$SDK_LIB_DIR" ldd "$GE_BIN" \
  | awk '/=>/ {print $3}' \
  | grep -F "$SDK_LIB_DIR/" || true)"
[ -n "$SDK_SO_LIST" ] || die "resolved zero SDK libraries under $SDK_LIB_DIR — the bundle would ship with no .so files (check SDK_LIB_DIR and the build output)"
printf '%s\n' "$SDK_SO_LIST" | while read -r so; do cp -v "$so" "$BUNDLE/"; done
cat > "$BUNDLE/run.sh" <<'EOS'
#!/usr/bin/env sh
DIR="$(cd "$(dirname "$0")" && pwd)"
LD_LIBRARY_PATH="$DIR" exec "$DIR/ge" --game_data_root="${GE_GAME_DATA:-$DIR/assets}" "$@"
EOS
chmod +x "$BUNDLE/run.sh"
cat > "$BUNDLE/README.txt" <<EOS
GoldenEye Recomp $TAG — Linux amd64

Run:   ./run.sh                          (expects game data in ./assets)
   or: GE_GAME_DATA=/path/to/assets ./run.sh

You must supply your own legally-owned GoldenEye 007 game data. No copyrighted
game assets are included in this build.
EOS

# Build-only mode stops here — unconditionally, before any of the publish-side effects
# below (tar/push/tag/gh release). This is a hard `exit 0`, not a warning: there is no
# code path from here to `tar`/`git push`/`git tag`/`gh release create` when
# USE_CONTAINER=0, because this branch always returns before reaching them.
if [ "$USE_CONTAINER" -eq 0 ]; then
  step "Build-only (--no-container): stopping before the publish steps"
  echo "Bundle: $BUNDLE"
  echo "NOTE: this bundle is linked against THIS HOST's glibc/libstdc++, not the release" >&2
  echo "      container's floor — it will NOT run on Steam Deck or older distros and must" >&2
  echo "      NOT be published (issue #12). Local testing only." >&2
  exit 0
fi

# Verify portability BEFORE the tag is pushed, so a failure never leaves a dangling tag
# (same reasoning as the build-before-tag ordering above).
step "Verifying ABI floor + bundle load + build parity"
run_gate "check-abi-floor.sh" "release binaries exceed the ABI floor — see issue #12" \
  "$ROOT/scripts/check-abi-floor.sh" "$BUNDLE"/ge "$BUNDLE"/*.so
run_gate "smoke-test-bundle.sh" "bundle failed to resolve its libraries on stock ubuntu:24.04" \
  "$ROOT/scripts/smoke-test-bundle.sh" "$BUNDLE"
# Container builds silently drop SDL3 backends (audio, udev) whose -dev packages are missing
# from the build image — a distinct failure mode from the ABI gate above, and the one that
# shipped a boot-deadlocking, audio-less build once already (see docker/linux-release.Dockerfile).
run_gate "check-build-parity.sh" "release build is missing capabilities the native build has — see docker/linux-release.Dockerfile" \
  "$ROOT/scripts/check-build-parity.sh" "$SDK_LIB_DIR/librexruntimerd.so"

TARBALL="$DIST/GoldenEye-Recomp-$TAG-linux-amd64.tar.gz"
tar -C "$BUNDLE" -czf "$TARBALL" .

# --- push provenance (both repos) + tag (only now that builds succeeded) ----
step "Pushing commits + tag"
git -C "$SDK_DIR" push
git push
MAIN_SHA="$(git rev-parse --short HEAD)"
SDK_SHA="$(git -C "$SDK_DIR" rev-parse --short HEAD)"
git tag -a "$TAG" -m "GoldenEye Recomp $TAG"
git push origin "$TAG"

# --- release notes ----------------------------------------------------------
step "Writing release notes"
NOTES="$DIST/notes.md"
{
  echo "## GoldenEye Recomp $TAG"
  echo
  if [ "$PRERELEASE" -eq 1 ]; then
    echo "Prerelease build of the Android handheld port + stability fixes."
  else
    echo "Release build of the Android handheld port + Linux desktop build."
  fi
  echo
  echo "### Artifacts"
  echo "- \`GoldenEye-Recomp-$TAG-android-arm64.apk\` — signed Android APK (arm64-v8a)."
  echo "- \`GoldenEye-Recomp-$TAG-linux-amd64.tar.gz\` — Linux amd64 bundle (\`run.sh\` + libs)."
  echo
  echo "### Requirements"
  echo "- Android: arm64 device with an **Adreno (Qualcomm)** GPU (needs Vulkan \`vertexPipelineStoresAndAtomics\`; Mali is unsupported)."
  echo "- You must supply your own legally-owned GoldenEye 007 game data. No copyrighted assets are shipped."
  echo
  echo "### Changes"
  if [ -n "$PREV_TAG" ]; then
    git log --no-merges --pretty='- %s' "$PREV_TAG"..HEAD
  else
    git log --no-merges --pretty='- %s' -20
  fi
  echo
  echo "### Provenance"
  echo "- game: \`$MAIN_SHA\` (branch \`$(git rev-parse --abbrev-ref HEAD)\`)"
  echo "- rexglue SDK: \`$SDK_SHA\`"
} > "$NOTES"

# --- publish ----------------------------------------------------------------
# Resolve the target repo from origin explicitly: with both origin + upstream
# remotes and no default set, `gh release create` fails with a misleading
# "workflow scope may be required" error. The tag is already pushed, so no
# --target is needed.
step "Publishing GitHub release"
REPO="$(git remote get-url origin | sed -E 's#^.*[:/]([^/]+/[^/]+)$#\1#; s#\.git$##')"
PRE_FLAG=""; [ "$PRERELEASE" -eq 1 ] && PRE_FLAG="--prerelease"
gh release create "$TAG" $PRE_FLAG \
  --repo "$REPO" \
  --title "GoldenEye Recomp $TAG" \
  --notes-file "$NOTES" \
  "$APK_OUT" "$TARBALL"

step "Done"
gh release view "$TAG" --repo "$REPO" --json url,isPrerelease,assets \
  -q '"url=" + .url + "  prerelease=" + (.isPrerelease|tostring) + "  assets=" + ([.assets[].name]|join(", "))'
