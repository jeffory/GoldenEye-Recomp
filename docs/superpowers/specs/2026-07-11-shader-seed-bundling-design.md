# Bundled Shader-Storage Seed (Phase 2B, part 1) — Design

**Date:** 2026-07-11
**Repos touched:** game only (`android/`, `scripts/`; no SDK changes)
**Status:** Approved
**Evidence base:** `docs/superpowers/2026-07-11-first-shot-hitch-attribution.md` — the first-shot
hitch is blocking Adreno pipeline compilation (~18ms/pipeline, worst 232ms frame); a warm
driver cache eliminates it; the guest `.xsh`/`.xpso` storage drives a boot-time precompile.

## Problem

A fresh install has empty shader/pipeline storage, so every shader translation and pipeline
compile happens lazily mid-gameplay — the first shot at each enemy type stalls the CP thread
for tens to hundreds of ms while Adreno compiles the new hit-effect pipelines. All of that
work is precompilable: the SDK already bulk-recreates every pipeline described in
`584108A9.fbo.vk.xpso` at title launch (behind the Android boot-loader screen), and item 1's
debounce then persists the resulting driver blob. The only missing piece is shipping grown
storage to first-install users.

## Design

### 1. Seed artifacts (committed)

`android/app/src/main/assets/shader_seed/584108A9.xsh` and
`android/app/src/main/assets/shader_seed/584108A9.fbo.vk.xpso`, pulled from the Ayn Thor
(which has menu + Dam-combat coverage as of 2026-07-11). ~100–300KB total, committed as
binary files. Safety properties (all existing SDK behavior, no changes needed):

- Guest-portable: Xenos microcode + pipeline descriptions keyed by title id — no driver or
  host-arch data.
- Version-headered: on a storage-format bump the SDK truncates and rebuilds; a stale seed is
  ignored, never a crash.
- Backend-variant-named (`.fbo` vs `.fsi`): a device using the FSI path would simply not find
  a matching seed — benign no-op. Both the Thor (Adreno) and desktop RADV use `.fbo`.

### 2. Refresh workflow

- New `scripts/refresh-shader-seed.sh`: pulls the two files from the Thor
  (`adb [-s <serial>] pull /sdcard/Android/data/com.sunjaycy.goldeneye/files/cache/shaders/shareable/<file>`)
  into `android/app/src/main/assets/shader_seed/`, prints old→new sizes, reminds to commit.
  Device serial defaults to the sole attached device; `-s` forwarded via `$ANDROID_SERIAL` or
  first argument.
- `scripts/cut-release.sh`: before building, warn (not fail) if either seed file's git commit
  date is older than 30 days: "shader seed is N days old — consider scripts/refresh-shader-seed.sh".

### 3. First-boot copy (Java)

In `GoldenEyeActivity`, before the boot loader starts: for each asset listed under
`shader_seed/`, if `<external-files>/cache/shaders/shareable/<name>` does **not** exist,
stream-copy it (creating parent dirs). Rules:

- **Copy-if-absent only.** Devices with existing storage keep their own grown files (an
  updated APK's richer seed does not overwrite; the device converges through its own play).
- Copy to a `<name>.tmp` sibling then rename, so a mid-copy kill can't leave a truncated file
  that shadows the seed forever (the SDK would reject a truncated file via its header check,
  but rename-atomicity costs two lines).
- Failures (missing assets dir, IO errors) log a warning and continue — the game boots
  seedless, exactly like today. Never fatal.
- Log one info line per copied file (`shader seed: copied <name> (<bytes> bytes)`) so
  on-device verification is a logcat/ge.log grep.

### 4. Runtime effect (no code changes)

Fresh install → seed copied → SDK precompiles all seeded pipelines at title launch behind the
loader screen (session-3 capture proved the loader's `rendered#` gating tolerates a
full-storage cold-driver boot) → item-1 debounce writes the driver `.vk.pcache` within ~10s →
combat first-shot compiles are cache hits (~0.34ms/pipeline).

## Testing (Thor, using the 2A counters as the instrument)

1. **Fresh-install simulation:** uninstall app, reinstall new APK, wipe nothing else needed
   (uninstall clears external files — restore game data per the known tar-push recipe), boot:
   verify the copy log lines, both files present before title launch, boot reaches
   `rendered#`, and note boot duration vs a seedless boot.
2. **Acceptance:** play Dam, shoot fresh guard types: no GESPIKE with `pcomp` > 10ms during
   combat (boot/load-screen spikes are fine).
3. **Existing-install no-op:** install over a device with grown storage: file mtimes
   unchanged, no copy log lines.
4. Release-build packaging check: `assets/shader_seed/` present in the release APK
   (aapt/unzip listing) — guards against a flavor-specific assets misconfiguration.

## Out of scope (future)

- Linux bundle seeding (same idea via cut-release tarball; separate small spec if wanted).
- Merging device-grown storage back into the seed (append-log merge tooling).
- Budgeting the `EndSubmission` compile drain + `creation_completion_event_` wait — Phase 2B
  part 2, separate spec; the reviewer flagged the wait as part of the unattributed CP time.

## Risks

Low. Additive Java copy + committed data files + two scripts. Worst case: corrupt/stale seed
→ SDK header validation discards it → behavior identical to today. The committed binaries add
~300KB to the repo and APK.
