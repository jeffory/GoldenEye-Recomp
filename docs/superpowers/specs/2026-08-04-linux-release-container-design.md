# Containerized Linux Release Build (Steam Deck Compatibility) — Design

**Date:** 2026-08-04
**Repos touched:** game only (`docker/`, `scripts/`, `docs/RELEASING.md`) — no SDK or source changes
**Status:** Approved
**Closes:** issue #12 ("Can't launch on Steam Deck")

## Problem

`scripts/cut-release.sh:120` builds the Linux amd64 bundle natively on the Fedora 44
workstation (glibc 2.43, GCC 16.1.1) and bundles only the SDK's own `.so` files. libc, libm
and libstdc++ come from the player's system, so every shipped tarball silently inherits the
build host's symbol-version floor and cannot load on any older distro. A Steam Deck user
(#12) gets:

```
/usr/lib/libm.so.6: version `GLIBC_2.43' not found (required by ge)
/usr/lib/libm.so.6: version `GLIBC_2.43' not found (required by librexruntimerd.so)
/usr/lib/libstdc++.so.6: version `GLIBCXX_3.4.35' not found (required by librexruntimerd.so)
```

Measured on the shipped v1.6.0 artifact with `objdump -T dist/bundle/{ge,librexruntimerd.so}`,
the break is exactly **seven symbols** in two groups:

| Group | Symbols | Cause |
|---|---|---|
| `libm` @ `GLIBC_2.43` | `sqrtf` (in `ge`); `acosf`, `asinf`, `atan2f`, `log10f` (in `librexruntimerd.so`) | glibc 2.43 version-bumped a batch of float math functions; the `@GLIBC_2.2.5` originals still exist everywhere |
| `libstdc++` @ `GLIBCXX_3.4.35` | `std::__detail::__wait_impl`, `__notify_impl`, `__wait_args::_M_setup_proxy_wait` | GCC 16 moved `std::atomic::wait/notify` out-of-line into libstdc++; the SDK uses it in ~10 files |

`libc.so.6` itself needs only `GLIBC_2.38`, so those seven symbols are the entire barrier.

### Rejected alternatives

- **`.symver` pinning via a force-included header.** Measured with clang 20 on this host:
  `.symver` on an *undefined* symbol is strictly per-TU **and** must appear *after* the
  reference. A separate stub TU emitting the directives has zero effect on other TUs, and
  `-include` can only prepend. The asm-label variant
  (`extern "C" float f(float) __asm__("sqrtf@GLIBC_2.2.5")` plus macros) leaks because
  `<cmath>`'s C++ overloads bypass the macros. Making it work would mean appending directives
  to the bottom of every TU touching those functions, in both repos, permanently.
- **`-static-libstdc++`.** `ge` and `librexruntimerd.so` exchange C++ objects and exceptions;
  two static libstdc++ copies break RTTI and exception matching across the DSO boundary. It
  also does nothing for the libm half.
- **Bundling the host `libstdc++.so.6`.** Fedora 44's libstdc++ itself requires
  `GLIBC_ABI_GNU2_TLS`, which pushes the floor back up to roughly glibc 2.41. Circular.

Building against an older toolchain is the only approach that fixes both groups at once,
needs no source changes, and carries no ABI risk.

## Architecture

Four new pieces in the game repo, composed by the existing release script. Nothing in the
SDK or in any `.cpp` changes.

```
scripts/cut-release.sh
  ├─ Android APK          (native NDK — unchanged)
  ├─ scripts/build-linux-container.sh   ← replaces the native cmake step
  │     └─ podman run  goldeneye-linux-release:noble
  ├─ scripts/check-abi-floor.sh         ← hard gate
  ├─ link smoke test      (clean ubuntu:24.04, runtime libs only)
  └─ tarball, tag, notes, upload        (unchanged)
```

### 1. Container image — `docker/linux-release.Dockerfile`

Built locally, tagged `goldeneye-linux-release:noble`.

> **Base image history.** The original plan (Task 1-3 below) chose `debian:12`. That build
> failed for a real reason, not a configuration mistake: the SDK specializes
> `std::chrono::clock_time_conversion` for its custom clocks, and Debian 12's `libstdc++-12`
> does not declare that template at all (zero occurrences in `/usr/include/c++/12/chrono`) —
> compilation fails with "explicit specialization of undeclared template", not a missing
> symbol. The documented fallback, `libstdc++-13` from bookworm-backports, does not exist:
> `apt-cache policy libstdc++-13-dev` returns an empty candidate on bookworm. The human
> partner chose **Ubuntu 24.04** instead, which ships `g++-13`/`libstdc++-13-dev` in main —
> no PPA, no backports — and does declare the template. This is recorded here so the base
> image is not "helpfully" moved back to Debian by a future reader of the original rationale
> below.

- **Base `ubuntu:24.04`** — glibc 2.39, `libstdc++-13` → `GLIBCXX_3.4.33`. Comfortably below
  SteamOS and every currently-supported distro, while still new enough for the codebase's
  C++23 (including the `std::chrono::clock_time_conversion` specialization Debian 12 could
  not compile).
- **clang-19 + lld-19 from apt.llvm.org/noble.** Ubuntu 24.04's stock clang cannot do
  `-std=c++23` the way the project needs. The presets already select `clang`/`clang++`, so a
  `update-alternatives` symlink is enough — `CMakePresets.json` is not modified.
- **`g++-13` / `libstdc++-13-dev`** — the target compiler and libstdc++ headers. This is the
  package that actually fixes the chrono build failure; Ubuntu 24.04 ships it in `main` as
  the default GCC, so no PPA or backports repo is needed.
- **`libgtk-3-dev`** — the *only* external system dependency
  (`rexglue_helpers.cmake:26-27`, `pkg_check_modules(GTK3 REQUIRED gtk+-3.0)`). It pulls the
  X11/Wayland/pango/cairo dev chain transitively. SDL3, Vulkan-Headers, volk, FFmpeg,
  glslang, SPIRV-Tools and imgui are all vendored submodules;
  `find_package(Vulkan QUIET)` is optional and Vulkan is loaded via volk at runtime.
- **`ninja-build`, `pkg-config`, `python3`.**
- **CMake 3.31 from the official binary tarball.** Ubuntu 24.04 ships 3.28.3, which only
  barely satisfies the project's `cmake_minimum_required(VERSION 3.25)`.

The image is content-stable; the script builds it on first use and reuses it thereafter.

### 2. Build script — `scripts/build-linux-container.sh`

Runs the build under **podman** (rootless, and the Fedora-native choice; docker is present
but not required). Usage:

```
scripts/build-linux-container.sh [--sdk DIR] [--rebuild-image]
```

Mount layout — the isolation is the substantive part of this script:

| Host path | Container path | Purpose |
|---|---|---|
| game repo | `/work` | sources plus `generated/` PPC code |
| `$SDK_DIR` | `/sdk` | built via `add_subdirectory` (`generated/rexglue.cmake:11`) |
| `$SDK_DIR/out-container/` | `/sdk/out` | **shadowing bind mount** |
| `out/build/linux-amd64-container/` | same, under `/work` | separate CMake cache |

The `/sdk/out` entry is a second bind mount layered over the already-mounted `/sdk`, so that
path inside the container resolves to the host's `out-container/` rather than to the SDK
repo's own `out/`. It exists because the SDK hardcodes its output directory to
`${REXGLUE_ROOT}/out/${REX_PLATFORM}` (SDK `CMakeLists.txt:191-193`) — a shared
location *outside* any build tree. Without that shadowing, a release build overwrites the
`out/linux-amd64/librexruntimerd.so` that native dev builds and `LD_LIBRARY_PATH` runs
depend on, and every switch between native and release forces a full relink.

Inside the container the script configures with `-B /work/out/build/linux-amd64-container`,
`-DREXSDK_DIR=/sdk`, `-DCMAKE_BUILD_TYPE=RelWithDebInfo` and `-G Ninja`, then builds target
`ge`. RelWithDebInfo is retained deliberately: it keeps symbols for crash diagnosis, matching
today's release.

`CMakeLists.txt:69` hints libatomic at `/usr/lib/gcc/x86_64-redhat-linux/16`; in the
container that `find_library` misses and the existing `-latomic` fallback (line 73) applies.
No change needed.

Output: `out/build/linux-amd64-container/GoldenEye` plus
`$SDK_DIR/out-container/*.so` for the bundle step.

### 3. ABI floor gate — `scripts/check-abi-floor.sh`

```
scripts/check-abi-floor.sh <binary> [<binary> ...]
```

Parses the `Version References` block of `objdump -p` for each input, extracts every
required `GLIBC_x.y` and `GLIBCXX_x.y.z`, and exits non-zero if any exceeds the ceiling
(`GLIBC 2.39`, `GLIBCXX 3.4.33`, overridable by env var). Version comparison is numeric per
component, not lexical — `GLIBC_2.9` must not compare greater than `GLIBC_2.39`. Failure
prints the offending symbols, obtained via `objdump -T | grep`, so the regression is
immediately actionable.

This is the durable part of the change: it is precisely the check that would have caught #12
before upload, and it fails loudly at release time when future SDK code pulls in a newer
symbol, instead of failing silently on a player's device.

### 4. Link smoke test

Runs the assembled bundle under a clean `ubuntu:24.04` container with only *runtime* packages
(`libgtk-3-0`, no `-dev`), and asserts `LD_LIBRARY_PATH=. ldd ge` reports no `not found`.
This demonstrates the tarball resolves on a stock old system without access to a Deck. It
verifies loading only — it does not launch the game, which needs a GPU and game assets.

### 5. `cut-release.sh` integration

The `step "Building Linux amd64"` block (lines 118-124) calls `build-linux-container.sh`
instead of `cmake --build --preset linux-amd64-relwithdebinfo`, and the dependency-copy loop
(lines 131-134) reads from `$SDK_DIR/out-container/` instead of `$SDK_DIR/out/linux-amd64`.
The floor gate and smoke test run after bundle assembly and before the tag is pushed, so a
failure never leaves a dangling tag — consistent with the script's existing
build-before-tag ordering (lines 91-93).

A new `--no-container` flag restores the current native path verbatim, for local iteration
and as an escape hatch if the image is unavailable. The Android APK step is untouched.

## Error handling

- Missing podman → fail in the preconditions block alongside the existing `gh` / keystore
  checks, not midway through a release.
- Image build failure → abort before the version-bump commit is created.
- Compile failure inside the container → propagates as a non-zero exit; the version-bump
  commit stays local and is recoverable with `git reset --hard HEAD~1` as documented at
  `cut-release.sh:91-93`.
- Floor gate or smoke test failure → abort before pushing the tag.

## Testing

A Steam Deck is available locally, so the fix is verified end-to-end on the actual target
rather than inferred from symbol tables alone.

**Baseline measurement (do first).** Record the Deck's actual `ldd --version` and its highest
`GLIBCXX_3.4.*` (`strings /usr/lib/libstdc++.so.6 | grep GLIBCXX_3.4 | sort -V | tail -1`).
This confirms the chosen `GLIBC 2.39` / `GLIBCXX 3.4.33` ceiling really sits below SteamOS,
and pins down the true floor for future reference. If SteamOS turns out to be *below* 2.39,
the ceiling drops accordingly and the base image is revisited.

**Measured 2026-08-04 over SSH on the Steam Deck (SteamOS, `deck@192.168.1.6`):**
`glibc 2.41` (`glibc 2.41+r65+ge7c419a29575-1`), `GLIBCXX_3.4.34`, `CXXABI_1.3.15`, from
`gcc-libs 15.1.1`. The originally-chosen 2.36 / 3.4.30 (Debian 12) ceiling sat five glibc
releases and four GLIBCXX revisions below the target. Debian 12 was later abandoned (see the
base-image history note under "Container image" above) in favor of Ubuntu 24.04's 2.39 /
3.4.33, which still sits comfortably below the Deck's 2.41 / 3.4.34 — two glibc releases and
one GLIBCXX revision of margin.

That measurement also corrects the version attribution above: the Deck runs GCC 15.1's
libstdc++ and tops out at `GLIBCXX_3.4.34`, and `objdump -T` on it finds **no**
`__wait_impl` symbol under any version. The out-of-line atomic wait/notify entry points are
therefore new in **GCC 16**, not merely re-versioned there — which is why a GCC 16 host
build cannot run on a GCC 15 target no matter how close the versions look.

1. `check-abi-floor.sh` against the **existing** `dist/bundle/{ge,librexruntimerd.so}` must
   FAIL, naming the seven known symbols. This proves the gate detects the real bug.
2. The same script against a trivial `ubuntu:24.04`-built binary must PASS.
3. Full container build produces `GoldenEye`; `objdump -p` shows max `GLIBC_2.39` /
   `GLIBCXX_3.4.33`; the gate passes.
4. Smoke test resolves all libraries in the clean runtime container.
5. Native `--no-container` path still produces a working build, and
   `$SDK_DIR/out/linux-amd64` is untouched by the container build (shadow-mount isolation
   holds).
6. The container-built binary runs on this Fedora 44 host (forward compatibility).
7. **On-device:** copy the bundle to the Deck, run `./run.sh` in Desktop Mode against real
   game data, and confirm it launches and renders. This is the acceptance criterion for
   closing #12 — everything above only proves the *linking* class of bug is fixed.

## Risks

- **RESOLVED — libstdc++-12 could not compile the SDK's C++23, and Debian 12 was
  abandoned as the base.** This risk was originally written expecting an *ABI-symbol*
  problem (a newer libstdc++ requiring a newer-than-floor `GLIBCXX_x.y.z`), with a
  documented fallback of pulling `libstdc++-13` from bookworm-backports. Task 4's actual
  container build hit something worse: the SDK specializes
  `std::chrono::clock_time_conversion` for its custom clocks, and Debian 12's `libstdc++-12`
  does not declare that template *at all* — `explicit specialization of undeclared
  template`, a compile-time hard failure, not a linkable-but-too-new symbol. The documented
  fallback also turned out not to exist: `apt-cache policy libstdc++-13-dev` returns an
  empty candidate on bookworm, so there was no in-place fix. The human partner chose to move
  the base image to **Ubuntu 24.04**, which ships `g++-13`/`libstdc++-13-dev` in `main` (no
  PPA, no backports) and does declare the template. The new floor is `GLIBC 2.39` /
  `GLIBCXX 3.4.33` — still comfortably under the Deck's `2.41` / `3.4.34` (see the Testing
  section) and under the `GLIBC_2.43` / `GLIBCXX_3.4.35` that broke us on Fedora 44 in the
  first place.
- **Launching on the Deck may surface non-ABI problems** unrelated to #12 (GPU/driver,
  controller mapping, asset paths). Deck hardware is available locally, so these are
  discoverable — but they are separate issues from the symbol-version fix and should not
  block it.
- **First container build is slow** (cold ccache-less full rebuild of SDK + game). Release
  builds only; day-to-day development stays on the fast native path.

## Out of scope

- Windows and Android release paths.
- Containerizing day-to-day development builds.
- Shipping a Flatpak or AppImage, or Steam Runtime (`sniper`) packaging. The floor chosen
  here makes the plain tarball work on the Deck in Desktop Mode, which is how the reporter
  and the README instruct users to run it.
