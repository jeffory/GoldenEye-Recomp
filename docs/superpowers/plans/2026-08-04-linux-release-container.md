# Containerized Linux Release Build (Steam Deck) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Ship Linux release bundles that load on Steam Deck (and any distro back to glibc 2.39) by building them in an Ubuntu 24.04 container instead of natively on Fedora 44, with an ABI floor gate that prevents the regression from recurring.

**Architecture:** Four new files in the game repo — a Dockerfile, a container build script, an ABI floor checker, and a bundle smoke test — composed by the existing `scripts/cut-release.sh`. No `.cpp` changes and no SDK changes. The build runs under rootless podman with a shadowing bind mount over `/sdk/out` so container artifacts never clobber the native dev build.

**Tech Stack:** podman (rootless), Ubuntu 24.04 (`ubuntu:24.04`), clang-19 + lld-19 from apt.llvm.org, g++-13/libstdc++-13, CMake 3.31, Ninja, GNU binutils (`objdump`), bash.

**Spec:** `docs/superpowers/specs/2026-08-04-linux-release-container-design.md`

> **Base image retooled 2026-08-04 (Task 4b):** this plan was originally written and mostly
> executed against **Debian 12** (`debian:12`, `GLIBC` ≤ 2.36, `GLIBCXX` ≤ 3.4.30, image tag
> `goldeneye-linux-release:bookworm`). Task 4's real container build failed: the SDK
> specializes `std::chrono::clock_time_conversion`, and Debian 12's `libstdc++-12` does not
> declare that template at all — a compile error, not a too-new-symbol problem — and the
> plan's documented fallback (`libstdc++-13` from bookworm-backports) does not exist on
> bookworm. The base moved to **Ubuntu 24.04**, which ships `g++-13`/`libstdc++-13-dev` in
> `main`. The values below are updated to match; the task narrative further down is kept
> as-executed for history, with values corrected in place.

## Global Constraints

- ABI floor for all shipped Linux binaries: **`GLIBC` ≤ 2.39**, **`GLIBCXX` ≤ 3.4.33**. Overridable via `ABI_MAX_GLIBC` / `ABI_MAX_GLIBCXX`.
- Base image is **`ubuntu:24.04`**. Do not substitute a different base without re-checking the floor against SteamOS.
- **No changes to any `.cpp`/`.h` file, to `CMakePresets.json`, or to the SDK repo.** The fix is entirely build-environment.
- Container runtime is **podman** (rootless). Docker is installed but is not a requirement.
- Use `--security-opt label=disable`, **never** `:z`/`:Z` mount flags — the latter recursively relabel the user's source repos on SELinux.
- SDK path default: `/home/keith/Projects/GoldenEye-Recomp-rexglue`, overridable with `--sdk DIR`.
- Container build artifacts go to `out/build/linux-amd64-container/` (game) and `$SDK_DIR/out-container/` (SDK). The native `out/build/linux-amd64-relwithdebinfo/` and `$SDK_DIR/out/linux-amd64/` must remain untouched.
- Build type stays **RelWithDebInfo** (keeps symbols for crash diagnosis, matching today's release).
- All new scripts: `#!/usr/bin/env bash`, `set -euo pipefail`, `chmod +x`.

---

### Task 1: Measure the Steam Deck baseline

Establishes that the 2.39 / 3.4.33 ceiling actually sits below SteamOS. If the Deck turns out to be *below* 2.39, the whole base-image choice changes, so this runs first.

**Files:**
- Modify: `docs/superpowers/specs/2026-08-04-linux-release-container-design.md` (record measured values)

**Interfaces:**
- Consumes: nothing
- Produces: confirmed values for `ABI_MAX_GLIBC` / `ABI_MAX_GLIBCXX` used by Task 2

- [ ] **Step 1: Read the Deck's glibc and libstdc++ versions**

On the Steam Deck, in Desktop Mode, open Konsole and run:

```bash
ldd --version | head -1
strings /usr/lib/libstdc++.so.6 | grep -E '^GLIBCXX_3\.4' | sort -V | tail -1
strings /usr/lib/libstdc++.so.6 | grep -E '^CXXABI_1\.3' | sort -V | tail -1
```

Record all three lines.

- [ ] **Step 2: Confirm the ceiling is below SteamOS**

The measured glibc must be **≥ 2.39** and the measured `GLIBCXX_3.4.*` must be **≥ 3.4.33**.

- If both hold: the plan proceeds unchanged.
- If glibc < 2.39: STOP. Report to the user — the base image must drop to Ubuntu 22.04 (2.35) or the Steam Runtime `sniper` image (2.31), which changes Task 3.
- If `GLIBCXX` < 3.4.33: STOP and report — `libstdc++-11` or `libc++` would be needed.

- [ ] **Step 3: Record the measurement in the spec**

Add to the "Testing" section of the spec, under the "Baseline measurement" paragraph, a line of the form:

```markdown
**Measured 2026-08-04 on the Steam Deck (SteamOS <version>):** glibc <X.YZ>, GLIBCXX_3.4.<N>,
CXXABI_1.3.<M>. The 2.39 / 3.4.33 ceiling therefore sits <D> glibc releases below the target.
```

- [ ] **Step 4: Commit**

```bash
git add docs/superpowers/specs/2026-08-04-linux-release-container-design.md
git commit -m "docs: record measured Steam Deck ABI baseline"
```

---

### Task 2: ABI floor checker

The regression gate. Built first (before anything that produces binaries) because the *existing* broken artifacts in `dist/bundle/` are a ready-made failing test case.

**Files:**
- Create: `scripts/check-abi-floor.sh`

**Interfaces:**
- Consumes: nothing
- Produces: `scripts/check-abi-floor.sh <binary> [<binary> ...]` — exit `0` when every binary is within the floor, `1` when one exceeds it (offending symbols printed to stderr), `2` on usage/tooling error. Reads `ABI_MAX_GLIBC` (default `2.39`) and `ABI_MAX_GLIBCXX` (default `3.4.33`).

- [ ] **Step 1: Write the failing test**

There is no unit-test framework for shell here, so the test is a script that asserts both directions. Create `scripts/tests/test-check-abi-floor.sh`:

```bash
#!/usr/bin/env bash
# Verifies check-abi-floor.sh both rejects a too-new binary and accepts a compliant one.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
CHECK="$ROOT/scripts/check-abi-floor.sh"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT

fails=0
note() { printf '%s\n' "$*"; }

# --- Case 1: the known-bad shipped v1.6.0 artifact must be REJECTED ----------
# It requires GLIBC_2.43 (sqrtf, acosf, asinf, atan2f, log10f) and
# GLIBCXX_3.4.35 (std::__detail::__wait_impl and friends). See issue #12.
BAD="$ROOT/dist/bundle/librexruntimerd.so"
if [ ! -f "$BAD" ]; then
  note "SKIP case 1: $BAD not present (rebuild a bundle to exercise it)"
else
  if out="$("$CHECK" "$BAD" 2>&1)"; then
    note "FAIL case 1: checker accepted a GLIBC_2.43 binary"; fails=1
  else
    for want in GLIBC_2.43 GLIBCXX_3.4.35 sqrtf; do
      case "$out" in *"$want"*) ;; *) note "FAIL case 1: output missing '$want'"; fails=1 ;; esac
    done
    [ "$fails" -eq 0 ] && note "ok case 1: rejected the known-bad bundle"
  fi
fi

# --- Case 2: a trivially-compliant binary must be ACCEPTED -------------------
# /bin/true from the host is ancient-ABI and always within any sane floor.
if "$CHECK" /bin/true >/dev/null 2>&1; then
  note "ok case 2: accepted /bin/true"
else
  note "FAIL case 2: checker rejected /bin/true"; fails=1
fi

# --- Case 3: version compare must be numeric, not lexical -------------------
# With a 2.9 ceiling, a binary needing GLIBC_2.34 must be rejected; lexical
# comparison would wrongly accept it because "2.34" < "2.9" as a string.
if ABI_MAX_GLIBC=2.9 "$CHECK" /bin/true >/dev/null 2>&1; then
  note "FAIL case 3: lexical comparison bug (2.34 treated as <= 2.9)"; fails=1
else
  note "ok case 3: numeric version comparison"
fi

# --- Case 4: missing file is a usage error (exit 2), not a pass -------------
"$CHECK" "$TMP/nope" >/dev/null 2>&1 && rc=0 || rc=$?
if [ "${rc:-0}" -eq 2 ]; then note "ok case 4: missing file -> exit 2"
else note "FAIL case 4: missing file gave exit ${rc:-0}, want 2"; fails=1; fi

[ "$fails" -eq 0 ] && note "ALL PASS" || note "FAILURES"
exit "$fails"
```

```bash
chmod +x scripts/tests/test-check-abi-floor.sh
```

- [ ] **Step 2: Run it to make sure it fails**

Run: `scripts/tests/test-check-abi-floor.sh`
Expected: FAIL — `scripts/check-abi-floor.sh: No such file or directory`.

- [ ] **Step 3: Write the checker**

Create `scripts/check-abi-floor.sh`:

```bash
#!/usr/bin/env bash
# Fail if any binary requires a glibc/libstdc++ symbol version above the release floor.
#
# Shipped Linux bundles link against the *player's* libc/libm/libstdc++, so the build
# host's symbol-version floor leaks into the release. Issue #12: a Fedora 44 build
# (glibc 2.43 / GCC 16) would not load on a Steam Deck. This gate catches that class of
# regression at release time instead of on a player's device.
#
# Usage: scripts/check-abi-floor.sh <binary> [<binary> ...]
# Env:   ABI_MAX_GLIBC   (default 2.39)     ABI_MAX_GLIBCXX (default 3.4.33)
set -euo pipefail

MAX_GLIBC="${ABI_MAX_GLIBC:-2.39}"
MAX_GLIBCXX="${ABI_MAX_GLIBCXX:-3.4.33}"

[ $# -gt 0 ] || { echo "usage: $0 <binary> [<binary> ...]" >&2; exit 2; }
command -v objdump >/dev/null 2>&1 || { echo "error: objdump not found (install binutils)" >&2; exit 2; }

# ver_gt A B -> true when A is strictly newer than B, compared component-wise.
# `sort -V` is what makes 2.9 < 2.39 and 3.4.9 < 3.4.33 come out right; a plain
# string compare gets both backwards.
ver_gt() {
  [ "$1" != "$2" ] && [ "$(printf '%s\n%s\n' "$1" "$2" | sort -V | tail -1)" = "$1" ]
}

fail=0
for bin in "$@"; do
  [ -f "$bin" ] || { echo "error: no such file: $bin" >&2; exit 2; }

  # The "Version References" block lists one required version per line, e.g.
  #     0x069691a3 0x00 22 GLIBC_2.43
  # Other lines in the block ("required from libm.so.6:") end in a library name
  # and fall through the case below untouched.
  reqs="$(objdump -p "$bin" | sed -n '/Version References:/,/^$/p' | awk '{print $NF}')"

  for req in $reqs; do
    case "$req" in
      GLIBC_[0-9]*)   name=GLIBC;   ver="${req#GLIBC_}";   max="$MAX_GLIBC" ;;
      GLIBCXX_[0-9]*) name=GLIBCXX; ver="${req#GLIBCXX_}"; max="$MAX_GLIBCXX" ;;
      # GLIBC_ABI_GNU2_TLS, CXXABI_*, GCC_* and block headers are not gated.
      *) continue ;;
    esac
    if ver_gt "$ver" "$max"; then
      echo "FAIL: $(basename "$bin") requires ${name}_${ver} (floor ${name}_${max})" >&2
      objdump -T "$bin" | grep -F "($req)" | awk '{print "        " $NF}' | sort -u >&2
      fail=1
    fi
  done
done

if [ "$fail" -ne 0 ]; then
  {
    echo
    echo "These binaries exceed the release ABI floor and will not load on older distros"
    echo "(this is what broke the Steam Deck in issue #12)."
    echo "Build the release in the container instead: scripts/build-linux-container.sh"
  } >&2
  exit 1
fi

echo "ABI floor OK (<= GLIBC_$MAX_GLIBC, GLIBCXX_$MAX_GLIBCXX): $*"
```

```bash
chmod +x scripts/check-abi-floor.sh
```

- [ ] **Step 4: Run the test and make sure it passes**

Run: `scripts/tests/test-check-abi-floor.sh`
Expected: `ALL PASS`, exit 0. Case 1 must print `ok case 1: rejected the known-bad bundle` — if it prints SKIP, the `dist/bundle/` artifacts are missing and the most important assertion did not run; restore them from the v1.6.0 release tarball before continuing.

- [ ] **Step 5: Eyeball the real failure output**

Run: `scripts/check-abi-floor.sh dist/bundle/ge dist/bundle/librexruntimerd.so`
Expected: exit 1, listing `GLIBC_2.43` with `sqrtf`/`acosf`/`asinf`/`atan2f`/`log10f` and `GLIBCXX_3.4.35` with the three `std::__detail::__wait_*` symbols — the exact seven symbols from issue #12.

- [ ] **Step 6: Commit**

```bash
git add scripts/check-abi-floor.sh scripts/tests/test-check-abi-floor.sh
git commit -m "build: add ABI floor gate for Linux release binaries (#12)"
```

---

### Task 3: Release container image

**Files:**
- Create: `docker/linux-release.Dockerfile`

**Interfaces:**
- Consumes: floor values confirmed in Task 1
- Produces: image tag `goldeneye-linux-release:noble` with `clang`, `clang++`, `ld.lld`, `cmake`, `ninja`, `pkg-config` on `PATH` and GTK3 dev headers installed; workdir `/work`

- [ ] **Step 1: Write the Dockerfile**

Create `docker/linux-release.Dockerfile`:

```dockerfile
# Build image for shipped Linux releases.
#
# Ubuntu 24.04 pins the ABI floor at glibc 2.39 / GLIBCXX_3.4.33 — below SteamOS (Steam Deck)
# and every currently-supported distro. Building releases here instead of on the Fedora 44
# host is the fix for issue #12. Do not bump the base image without re-checking the floor.
#
# See docs/superpowers/specs/2026-08-04-linux-release-container-design.md
FROM ubuntu:24.04

ARG LLVM_VERSION=19
ARG CMAKE_VERSION=3.31.6
ENV DEBIAN_FRONTEND=noninteractive

# libgtk-3-dev is the ONLY external dependency the build needs: the SDK's
# rexglue_helpers.cmake does pkg_check_modules(GTK3 REQUIRED gtk+-3.0). SDL3,
# Vulkan-Headers, volk, FFmpeg, glslang, SPIRV-Tools and imgui are all vendored
# submodules, and Vulkan itself is loaded through volk at runtime.
RUN apt-get update && apt-get install -y --no-install-recommends \
      ca-certificates wget gnupg \
      ninja-build pkg-config python3 git file binutils \
      g++-13 libstdc++-13-dev libgtk-3-dev \
 && rm -rf /var/lib/apt/lists/*

# Ubuntu 24.04's stock clang cannot do -std=c++23 the way the project needs; pull clang-19
# from apt.llvm.org's noble repo instead.
RUN wget -qO /usr/share/keyrings/llvm.asc https://apt.llvm.org/llvm-snapshot.gpg.key \
 && echo "deb [signed-by=/usr/share/keyrings/llvm.asc] http://apt.llvm.org/noble/ llvm-toolchain-noble-${LLVM_VERSION} main" \
      > /etc/apt/sources.list.d/llvm.list \
 && apt-get update \
 && apt-get install -y --no-install-recommends "clang-${LLVM_VERSION}" "lld-${LLVM_VERSION}" \
 && rm -rf /var/lib/apt/lists/* \
 && ln -sf "/usr/bin/clang-${LLVM_VERSION}"   /usr/bin/clang \
 && ln -sf "/usr/bin/clang++-${LLVM_VERSION}" /usr/bin/clang++ \
 && ln -sf "/usr/bin/ld.lld-${LLVM_VERSION}"  /usr/bin/ld.lld

# Ubuntu 24.04 ships cmake 3.28.3, which only barely satisfies the project's
# cmake_minimum_required(VERSION 3.25). Use a current upstream build instead.
RUN wget -qO /tmp/cmake.tar.gz \
      "https://github.com/Kitware/CMake/releases/download/v${CMAKE_VERSION}/cmake-${CMAKE_VERSION}-linux-x86_64.tar.gz" \
 && tar -xzf /tmp/cmake.tar.gz -C /opt \
 && ln -sf "/opt/cmake-${CMAKE_VERSION}-linux-x86_64/bin/cmake" /usr/bin/cmake \
 && rm /tmp/cmake.tar.gz

WORKDIR /work
```

- [ ] **Step 2: Build the image**

Run: `podman build -f docker/linux-release.Dockerfile -t goldeneye-linux-release:noble .`
Expected: builds clean. First run pulls `ubuntu:24.04` and takes several minutes.

- [ ] **Step 3: Verify the toolchain and the floor it produces**

This is the test that the image is fit for purpose — it proves `libstdc++-13` handles the C++23 the codebase actually uses (`std::expected`, `std::atomic::wait/notify`) *and* that what it emits is inside the floor.

```bash
cat > /tmp/abi-probe.cpp <<'EOF'
#include <expected>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <thread>
std::expected<int, const char*> f(bool ok) {
  if (!ok) return std::unexpected("no");
  return 42;
}
std::atomic<int> a{0};
volatile float x = 2.0f;
int main() {
  std::thread t([]{ a.store(1); a.notify_one(); });
  a.wait(0);
  t.join();
  printf("%d %f %f\n", f(true).value(), sqrtf(x), atan2f(x, x));
}
EOF
podman run --rm --security-opt label=disable -v /tmp:/t goldeneye-linux-release:noble \
  bash -c 'clang++ --version | head -1 && cmake --version | head -1 &&
           clang++ -std=c++23 -O2 /t/abi-probe.cpp -o /t/abi-probe && echo BUILD_OK'
scripts/check-abi-floor.sh /tmp/abi-probe
```

Expected: `clang version 19.x`, `cmake version 3.31.6`, `BUILD_OK`, then `ABI floor OK`.

**Historical note:** this step originally targeted `debian:12` with `libstdc++-12`, and its
documented fallback for a compile failure was "pull `libstdc++-13` from bookworm-backports and
set `ABI_MAX_GLIBCXX=3.4.32`." That fallback does not exist — `apt-cache policy
libstdc++-13-dev` returns an empty candidate on bookworm — and the real failure was worse than
a too-new-symbol problem anyway: the SDK's `std::chrono::clock_time_conversion` specialization
does not compile at all against `libstdc++-12`, because that template isn't declared there. See
the spec's Risks section ("RESOLVED — libstdc++-12 could not compile the SDK's C++23...") for
the full account. `libstdc++-13` is now the *primary* toolchain (via Ubuntu 24.04), not a
fallback.

- [ ] **Step 4: Commit**

```bash
git add docker/linux-release.Dockerfile
git commit -m "build: add Ubuntu 24.04 release container image (glibc 2.39 floor)"
```

(The commit that actually landed for this step, `023b1a3`, predates the Task 4b retool and
reads `"build: add Debian 12 release container image (glibc 2.36 floor)"` — real git history,
left as-is. The Dockerfile retool itself is a separate, later commit.)

---

### Task 4: Container build script

**Files:**
- Create: `scripts/build-linux-container.sh`

**Interfaces:**
- Consumes: image `goldeneye-linux-release:noble` (Task 3)
- Produces: `scripts/build-linux-container.sh [--sdk DIR] [--rebuild-image] [-j N]`. On success leaves the binary at `out/build/linux-amd64-container/GoldenEye` and the SDK shared objects in `$SDK_DIR/out-container/`. Both paths are fixed and callers derive them directly; the trailing `GE_BIN=` / `SDK_OUT=` lines it prints are informational only, for humans reading a release log.

- [ ] **Step 1: Write the script**

Create `scripts/build-linux-container.sh`:

```bash
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
```

```bash
chmod +x scripts/build-linux-container.sh
```

- [ ] **Step 2: Record the native artifacts' state, so isolation can be proved**

```bash
sha256sum ~/Projects/GoldenEye-Recomp-rexglue/out/linux-amd64/librexruntimerd.so > /tmp/native-sdk-before.txt
ls -la out/build/linux-amd64-relwithdebinfo/ > /tmp/native-build-before.txt
cat /tmp/native-sdk-before.txt
```

- [ ] **Step 3: Run the build**

Run: `scripts/build-linux-container.sh`
Expected: image reused from Task 3, CMake configures, Ninja builds, final output ends with `GE_BIN=.../out/build/linux-amd64-container/GoldenEye` and `SDK_OUT=.../out-container`. This is a cold full rebuild of SDK + game and will take a while.

If CMake fails on a missing C++23 library feature, that is the known `libstdc++-13` risk — apply the `libstdc++-13` fallback from Task 3 Step 3 and re-run.

- [ ] **Step 4: Verify the produced binaries are within the floor**

```bash
scripts/check-abi-floor.sh \
  out/build/linux-amd64-container/GoldenEye \
  ~/Projects/GoldenEye-Recomp-rexglue/out-container/librexruntimerd.so
```

Expected: `ABI floor OK`. This is the moment the issue-#12 bug is actually fixed — the same command against `dist/bundle/` still fails.

- [ ] **Step 5: Verify the native build was not clobbered**

```bash
sha256sum -c /tmp/native-sdk-before.txt
diff <(ls -la out/build/linux-amd64-relwithdebinfo/) /tmp/native-build-before.txt && echo "NATIVE TREE UNTOUCHED"
```

Expected: `OK` from `sha256sum -c` and `NATIVE TREE UNTOUCHED`. A failure here means the `/sdk/out` shadow mount is not working and the container overwrote the dev build's shared object.

- [ ] **Step 6: Verify forward compatibility on the host**

Run: `ldd out/build/linux-amd64-container/GoldenEye | grep -c 'not found' || echo "RESOLVES ON FEDORA 44"`
Expected: `RESOLVES ON FEDORA 44` — a binary built to an older floor must still run on the newer host.

- [ ] **Step 7: Commit**

```bash
git add scripts/build-linux-container.sh
git commit -m "build: add containerized Linux release build script"
```

---

### Task 5: Bundle link smoke test

**Files:**
- Create: `scripts/smoke-test-bundle.sh`

**Interfaces:**
- Consumes: an assembled bundle directory containing `ge` and the SDK `.so` files
- Produces: `scripts/smoke-test-bundle.sh <bundle-dir>` — exit `0` when every library resolves inside a stock `ubuntu:24.04` runtime, `1` when any is `not found`, `2` on usage error

- [ ] **Step 1: Write the script**

Create `scripts/smoke-test-bundle.sh`:

```bash
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
out="$(podman run --rm --security-opt label=disable -v "$BUNDLE":/bundle:ro ubuntu:24.04 \
  bash -euo pipefail -c '
    export DEBIAN_FRONTEND=noninteractive
    apt-get update -qq >/dev/null
    apt-get install -y -qq --no-install-recommends libgtk-3-0 >/dev/null
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
```

```bash
chmod +x scripts/smoke-test-bundle.sh
```

- [ ] **Step 2: Run it against the known-bad v1.6.0 bundle**

Run: `scripts/smoke-test-bundle.sh dist/bundle`
Expected: FAIL. Ubuntu 24.04's libm/libstdc++ cannot satisfy `GLIBC_2.43`/`GLIBCXX_3.4.35`, so `ldd` reports `not found`. This confirms the smoke test detects the real defect rather than passing vacuously.

- [ ] **Step 3: Assemble a bundle from the container build**

```bash
rm -rf /tmp/ge-bundle && mkdir -p /tmp/ge-bundle
cp out/build/linux-amd64-container/GoldenEye /tmp/ge-bundle/ge
LD_LIBRARY_PATH=~/Projects/GoldenEye-Recomp-rexglue/out-container \
  ldd out/build/linux-amd64-container/GoldenEye \
  | awk '/=>/ {print $3}' | grep -F "out-container/" \
  | while read -r so; do cp -v "$so" /tmp/ge-bundle/; done
ls -la /tmp/ge-bundle
```

- [ ] **Step 4: Run the smoke test against it**

Run: `scripts/smoke-test-bundle.sh /tmp/ge-bundle`
Expected: `SMOKE TEST OK: all libraries resolve on stock ubuntu:24.04`.

- [ ] **Step 5: Commit**

```bash
git add scripts/smoke-test-bundle.sh
git commit -m "build: add bundle link smoke test against stock ubuntu:24.04"
```

---

### Task 6: Wire into cut-release.sh and document

**Files:**
- Modify: `scripts/cut-release.sh:24-34` (arg parsing), `:49-60` (preconditions), `:118-134` (Linux build + bundle assembly)
- Modify: `docs/RELEASING.md`

**Interfaces:**
- Consumes: `scripts/build-linux-container.sh`, `scripts/check-abi-floor.sh`, `scripts/smoke-test-bundle.sh`
- Produces: `scripts/cut-release.sh <version> [--stable] [--allow-dirty] [--sdk DIR] [--no-container]`

- [ ] **Step 1: Add the `--no-container` flag**

In the `while [ $# -gt 0 ]` loop (`scripts/cut-release.sh:24-34`), add a case beside `--allow-dirty`:

```bash
    --no-container) USE_CONTAINER=0 ;;
```

and initialise it with the other defaults above the loop (near `ALLOW_DIRTY=0`):

```bash
USE_CONTAINER=1
```

Update the usage comment at the top of the file (`:8-16`) to list the flag:

```bash
#   --no-container Build the Linux bundle natively instead of in the Ubuntu 24.04
#                  container. The native build is pinned to this host's glibc and
#                  will NOT run on Steam Deck or older distros (issue #12).
```

- [ ] **Step 2: Add the podman precondition**

In the preconditions block (`scripts/cut-release.sh:49-60`), after the `gh auth status` check:

```bash
if [ "$USE_CONTAINER" -eq 1 ]; then
  command -v podman >/dev/null || die "podman not found (or pass --no-container)"
fi
```

Failing here means a missing tool aborts before the version-bump commit is created, matching how the other preconditions behave.

- [ ] **Step 3: Replace the Linux build step**

Replace `scripts/cut-release.sh:118-124` (from the `# --- build: Linux amd64` comment through the `GE_BIN` guard) with:

```bash
# --- build: Linux amd64 release bundle -------------------------------------
# Built in the Ubuntu 24.04 container by default: a native Fedora build inherits this
# host's glibc 2.43 / GLIBCXX_3.4.35 floor and will not load on a Steam Deck (#12).
if [ "$USE_CONTAINER" -eq 1 ]; then
  step "Building Linux amd64 in the release container (glibc 2.39 floor)"
  "$ROOT/scripts/build-linux-container.sh" --sdk "$SDK_DIR"
  GE_BIN="$(find out/build/linux-amd64-container -maxdepth 1 -type f \( -name GoldenEye -o -name ge \) | head -1)"
  SDK_LIB_DIR="$SDK_DIR/out-container"
else
  step "Building Linux amd64 NATIVELY (relwithdebinfo) — NOT portable to older distros"
  cmake --build --preset linux-amd64-relwithdebinfo --target ge
  # The CMake target is `ge` but its OUTPUT_NAME is `GoldenEye`, so the built file
  # is `GoldenEye` (older builds emitted `ge`). Accept either.
  GE_BIN="$(find out/build/linux-amd64-relwithdebinfo -maxdepth 1 -type f \( -name GoldenEye -o -name ge \) | head -1)"
  SDK_LIB_DIR="$SDK_DIR/out/linux-amd64"
fi
[ -n "$GE_BIN" ] && [ -f "$GE_BIN" ] || die "GoldenEye/ge binary not found"
```

- [ ] **Step 4: Point bundle assembly at the right SDK output dir**

In the bundle assembly block, replace the dependency-copy loop (`scripts/cut-release.sh:131-134`) so it uses `$SDK_LIB_DIR` rather than the hardcoded native path:

```bash
LD_LIBRARY_PATH="$SDK_LIB_DIR" ldd "$GE_BIN" \
  | awk '/=>/ {print $3}' \
  | grep -F "$SDK_LIB_DIR/" \
  | while read -r so; do cp -v "$so" "$BUNDLE/"; done
```

- [ ] **Step 5: Gate the release on the floor check and smoke test**

Immediately after the `chmod +x "$BUNDLE/run.sh"` line and before the `TARBALL=` line, insert:

```bash
# Verify portability BEFORE the tag is pushed, so a failure never leaves a dangling tag
# (same reasoning as the build-before-tag ordering above).
if [ "$USE_CONTAINER" -eq 1 ]; then
  step "Verifying ABI floor + bundle load"
  "$ROOT/scripts/check-abi-floor.sh" "$BUNDLE"/ge "$BUNDLE"/*.so \
    || die "release binaries exceed the ABI floor — see issue #12"
  "$ROOT/scripts/smoke-test-bundle.sh" "$BUNDLE" \
    || die "bundle failed to resolve its libraries on stock ubuntu:24.04"
else
  echo "WARNING: --no-container build; skipping ABI floor + smoke checks." >&2
  echo "         Do NOT publish this bundle — it is pinned to this host's glibc." >&2
fi
```

- [ ] **Step 6: Verify the script parses and the flag works**

```bash
bash -n scripts/cut-release.sh && echo "SYNTAX OK"
scripts/cut-release.sh --help
```

Expected: `SYNTAX OK` and help text listing `--no-container`.

- [ ] **Step 7: Document it**

Append this section to `docs/RELEASING.md`:

```markdown
## Linux builds and the ABI floor

Linux release bundles are built inside an Ubuntu 24.04 container, not on the host. A native
Fedora build links against this workstation's glibc 2.43 / GLIBCXX_3.4.35 and will not load
on a Steam Deck or any older distro — that was issue #12, where v1.6.0 failed with
`version 'GLIBC_2.43' not found`. The container pins the floor at **glibc 2.39 /
GLIBCXX_3.4.33**, below SteamOS and every currently-supported distro.

`cut-release.sh` does this automatically. Two checks gate the release, both before the tag
is pushed:

- `scripts/check-abi-floor.sh` — fails if any shipped binary needs a symbol version above
  the floor.
- `scripts/smoke-test-bundle.sh` — fails if the bundle cannot resolve its libraries inside
  a stock `ubuntu:24.04` runtime.

### If the floor gate fails

New code (usually in the SDK) pulled in a symbol that only exists in a newer glibc or
libstdc++. The failure output names the exact symbols. Either avoid the construct, or raise
the floor deliberately — bump `ABI_MAX_GLIBC` / `ABI_MAX_GLIBCXX`, change the base image in
`docker/linux-release.Dockerfile`, and re-verify on a real Steam Deck before publishing.

### Rebuilding the image

    scripts/build-linux-container.sh --rebuild-image

### --no-container

`cut-release.sh --no-container` restores the old native build for local iteration. Its
output is pinned to this host's glibc and **must never be published**; the script skips the
portability checks and prints a warning when you use it.

Design notes: `docs/superpowers/specs/2026-08-04-linux-release-container-design.md`
```

- [ ] **Step 8: Commit**

```bash
git add scripts/cut-release.sh docs/RELEASING.md
git commit -m "release: build Linux bundles in the Ubuntu 24.04 container by default (#12)"
```

---

### Task 7: Verify on the Steam Deck

The acceptance criterion. Everything before this proves the *linking* class of bug is fixed; only hardware proves the fix.

**Files:**
- Modify: `docs/superpowers/specs/2026-08-04-linux-release-container-design.md` (record the result)

**Interfaces:**
- Consumes: the bundle produced by Task 6
- Produces: a verified-on-hardware bundle, and the evidence needed to answer issue #12

- [ ] **Step 1: Produce a release-shaped bundle without publishing**

Assemble the bundle exactly as `cut-release.sh` does, but stop before tagging:

```bash
scripts/build-linux-container.sh
rm -rf /tmp/ge-deck && mkdir -p /tmp/ge-deck
cp out/build/linux-amd64-container/GoldenEye /tmp/ge-deck/ge
LD_LIBRARY_PATH=~/Projects/GoldenEye-Recomp-rexglue/out-container \
  ldd out/build/linux-amd64-container/GoldenEye \
  | awk '/=>/ {print $3}' | grep -F "out-container/" \
  | while read -r so; do cp -v "$so" /tmp/ge-deck/; done
cat > /tmp/ge-deck/run.sh <<'EOS'
#!/usr/bin/env sh
DIR="$(cd "$(dirname "$0")" && pwd)"
LD_LIBRARY_PATH="$DIR" exec "$DIR/ge" --game_data_root="${GE_GAME_DATA:-$DIR/assets}" "$@"
EOS
chmod +x /tmp/ge-deck/run.sh
scripts/check-abi-floor.sh /tmp/ge-deck/ge /tmp/ge-deck/*.so
scripts/smoke-test-bundle.sh /tmp/ge-deck
tar -C /tmp/ge-deck -czf /tmp/ge-deck.tar.gz .
```

- [ ] **Step 2: Copy it to the Deck with the game assets**

Transfer `/tmp/ge-deck.tar.gz` to the Deck (`scp`, USB, or Warpinator), extract it in Desktop Mode, and place the game data in `./assets` next to `run.sh` — or point `GE_GAME_DATA` at it.

- [ ] **Step 3: Confirm the loader is satisfied**

On the Deck, in the extracted directory:

```bash
LD_LIBRARY_PATH=. ldd ./ge | grep 'not found' && echo "STILL BROKEN" || echo "ALL LIBS RESOLVE"
```

Expected: `ALL LIBS RESOLVE`. This is the direct refutation of the issue-#12 error text.

- [ ] **Step 4: Launch it**

Run: `./run.sh`
Expected: the game launches and renders.

If it launches but misbehaves (GPU/driver, controller mapping, asset paths), that is a *separate* problem from #12 — record it as its own issue and do not block this work on it. If it fails with another `version ... not found`, capture the full text: the floor needs to drop further and the base image must be revisited.

- [ ] **Step 5: Record the result in the spec**

Append to the spec's Testing section:

```markdown
**Verified on Steam Deck 2026-08-04:** container-built bundle resolves all libraries and
launches in Desktop Mode. Issue #12 reproduction (`GLIBC_2.43` / `GLIBCXX_3.4.35` not found)
no longer occurs.
```

- [ ] **Step 6: Commit**

```bash
git add docs/superpowers/specs/2026-08-04-linux-release-container-design.md
git commit -m "docs: record Steam Deck verification of the container-built bundle"
```

---

## After the plan

Once Task 7 passes, the remaining work is a release and a reply:

- Cut a patch release (`scripts/cut-release.sh v1.6.1 --stable`) so there is a Deck-working Linux tarball to point at.
- Reply to issue #12: it was a build-host problem, not anything the reporter could install; the advice they were finding online (installing/upgrading glibc on SteamOS) would have broken their Deck. Point them at the new tarball.
