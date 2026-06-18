#!/usr/bin/env bash
# Cut a GoldenEye-Recomp release: build the signed Android APK + a Linux amd64
# bundle locally, push the version-bump commit + tag, and publish a GitHub release.
#
# Public CI cannot build the real artifacts (they embed PPC code generated from
# your own GoldenEye 007 XEX), so releases are built locally and uploaded here.
#
# Usage:
#   scripts/cut-release.sh v1.3.0-android.1 [--stable] [--allow-dirty] [--sdk DIR]
#
#   <version>      Required. Tag/name, e.g. v1.3.0-android.1 (leading 'v' optional
#                  in versionName, kept in the git tag).
#   --stable       Publish as a full release (default: --prerelease).
#   --allow-dirty  Skip the clean-working-tree check.
#   --sdk DIR      Path to the ReXGlue SDK checkout
#                  (default: /home/keith/Projects/GoldenEye-Recomp-rexglue).
set -euo pipefail

# --- args -------------------------------------------------------------------
VERSION=""
PRERELEASE=1
ALLOW_DIRTY=0
SDK_DIR="/home/keith/Projects/GoldenEye-Recomp-rexglue"
while [ $# -gt 0 ]; do
  case "$1" in
    --stable) PRERELEASE=0 ;;
    --allow-dirty) ALLOW_DIRTY=1 ;;
    --sdk) SDK_DIR="$2"; shift ;;
    -h|--help) sed -n '2,20p' "$0"; exit 0 ;;
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

# --- preconditions ----------------------------------------------------------
step "Preconditions"
command -v gh >/dev/null   || die "gh CLI not found"
gh auth status >/dev/null 2>&1 || die "gh not authenticated (gh auth login)"
[ -d "$ROOT/generated" ]   || die "generated/ missing — run 'rexglue codegen' against your XEX first"
[ -f "android/keystore.properties" ] || die "android/keystore.properties missing — see docs/RELEASING.md (keytool genkeypair)"
[ -f "android/release.jks" ]         || die "android/release.jks missing — see docs/RELEASING.md"
if [ "$ALLOW_DIRTY" -eq 0 ]; then
  [ -z "$(git status --porcelain)" ] || die "working tree dirty (commit/stash, or pass --allow-dirty)"
fi
git rev-parse "$TAG" >/dev/null 2>&1 && die "tag $TAG already exists"
echo "version=$VNAME tag=$TAG prerelease=$PRERELEASE sdk=$SDK_DIR"

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

PREV_TAG="$(git describe --tags --abbrev=0 2>/dev/null || true)"

# --- version bump -----------------------------------------------------------
step "Bumping versionName -> $VNAME, versionCode++"
CUR_CODE="$(grep -oE 'versionCode[[:space:]]+[0-9]+' "$GRADLE_PROPS" | grep -oE '[0-9]+')"
NEW_CODE=$((CUR_CODE + 1))
sed -i -E "s/versionCode[[:space:]]+[0-9]+/versionCode $NEW_CODE/" "$GRADLE_PROPS"
sed -i -E "s/versionName[[:space:]]+'[^']*'/versionName '$VNAME'/" "$GRADLE_PROPS"
grep -E 'versionCode|versionName' "$GRADLE_PROPS"
git add "$GRADLE_PROPS"
git commit -q -m "chore(release): $TAG"

# Build BEFORE pushing/tagging so a build failure never leaves a dangling tag.
# The version-bump commit above stays local until the builds succeed; if a build
# fails, reset it with: git reset --hard HEAD~1

# --- build: Android signed release APK -------------------------------------
rm -rf "$DIST"; mkdir -p "$DIST"
APK_OUT="$DIST/GoldenEye-Recomp-$TAG-android-arm64.apk"
step "Building signed Android release APK"
( cd android && ./gradlew :app:assembleRelease -PrexSdkDir="$SDK_DIR" )
SIGNED_APK="android/app/build/outputs/apk/release/app-release.apk"
[ -f "$SIGNED_APK" ] || die "expected signed APK at $SIGNED_APK (unsigned build? check keystore.properties)"
cp "$SIGNED_APK" "$APK_OUT"

# --- build: Linux amd64 release bundle -------------------------------------
step "Building Linux amd64 (ge, relwithdebinfo — keeps symbols for crash diagnosis)"
cmake --build --preset linux-amd64-relwithdebinfo --target ge
GE_BIN="$(find out/build/linux-amd64-relwithdebinfo -name ge -type f | head -1)"
[ -n "$GE_BIN" ] && [ -f "$GE_BIN" ] || die "ge binary not found under out/build/linux-amd64-relwithdebinfo"

step "Assembling Linux bundle (ge + resolved .so deps)"
BUNDLE="$DIST/bundle"
mkdir -p "$BUNDLE"
cp "$GE_BIN" "$BUNDLE/ge"
# Copy every dependency ldd resolves out of the SDK out dir (rd/non-rd agnostic).
LD_LIBRARY_PATH="$SDK_DIR/out/linux-amd64" ldd "$GE_BIN" \
  | awk '/=>/ {print $3}' \
  | grep -F "$SDK_DIR/out/linux-amd64/" \
  | while read -r so; do cp -v "$so" "$BUNDLE/"; done
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
  echo "Prerelease build of the Android handheld port + stability fixes."
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
