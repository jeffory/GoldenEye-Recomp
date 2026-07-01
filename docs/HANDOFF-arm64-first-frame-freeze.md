# Handoff: arm64 (Android) first-frame freeze — GPU completion never delivered

> **✅ RESOLVED 2026-06-17 — verified on the Ayn Thor (9/9 first-attempt cold boots, 30s soak @ ~60fps, 0 stalls).**
> The fix is NOT "deliver the missing completion." The handoff's lead diagnosis below — *"completion
> counter `idblk+0` stuck at 0"* — was a **misread**: `idblk+0` is the WAIT_REG_MEM CPU↔GPU semaphore
> (0 = released), and the watchdog's `submit > completed` test trivially read "GPU BEHIND" against it.
> The real cause was the **arm64 blocking wait `rex_ge_cp_wait_progress`** itself (exactly what the
> desktop bisect `9dd0258` pinned): its ~2ms re-check cadence let `ge_dbg_now`'s 80ms skip-bit fallback
> latch, skipping render forever. The blocking wait only existed to stop yield-spinning guest threads
> starving the single CP worker on few-core handhelds — so the fix **boosts the CP worker to
> kAboveNormal at creation** (`command_processor.cpp`, `creation_flags 0 → 0x20`, applied un-gated by the
> `XThread::Create` priority path) and **unifies both arches on `std::this_thread::yield()`** (removed the
> `#if __aarch64__` blocking-wait branch + dead `cp_seq` plumbing in `ge_dbg_now`). Cleanups: the watchdog
> `completion=`/`completed=` diagnostic was relabeled `semaphore=`/`render=` (no longer infers "GPU
> behind" from the semaphore); and the Android loader now gates on a freeze-proof `GEGPU rendered#N`
> heartbeat (emitted only from the drawn branch, keyed on real frame advancement) instead of `present#`
> (which advances even on a frozen boot). The detailed evidence/hypotheses below are kept for history;
> treat the resolution above as authoritative.

**Audience:** a fresh Claude session picking up *only* the arm64 GPU-completion freeze.
**Status:** ✅ FIXED & verified (see banner above). Original status when handed off: root cause localized, fix NOT done.
**Date handed off:** 2026-06-17. Target device: **Ayn Thor handheld** (Adreno/Qualcomm, arm64-v8a, NDK 27.1).

---

## TL;DR

On Android the game boots, plays audio, renders the first frame (Twycross legal screen), then **freezes on that frame**. The guest main thread keeps cycling at ~60fps but render is skipped every frame because the **GPU completion counter never advances** (`completed`/`idblk+0` stays 0 while `submit` climbs). The CP ring drains (rpi==wpi) but the completion is never delivered back to the guest, so the frame loop's GPU-completion gate skips rendering forever.

This is the **same first-screen freeze** that hit desktop from commit `90755ad`. It was fixed on desktop in `9dd0258` by *arch-gating* (desktop restored `std::this_thread::yield()`); **arm64 deliberately kept the blocking wait `rex_ge_cp_wait_progress`** because plain yield starved the CP worker on low-core handhelds. So arm64 is on the un-fixed branch. It is **intermittent** (~50% of boots; the "GPU-ring race" in the project memory) — some boots render fine and run for minutes.

The goal: make the GPU completion actually get delivered on arm64 (so `completed` reaches `submit` and the render gate stops skipping), without reintroducing the CP-worker starvation that the blocking wait was added to fix.

---

## Confirmed evidence (freeze watchdog, captured on device)

Enable the watchdog (see "Enabling diagnostics on Android" below). The stall signature on the frozen Twycross screen:

```
GEWATCHDOG STALL: ring rpi=0x5d1 wpi=0x5d1 [DRAINED] | present#=267 (+75/stall) |
  dbgnow_polls=2140119 (+0/stall) | submit=139 completed=0 target=561 presented=35 skipbit=2 | dev=0x40001a80 idblk=0xffc9b000
GEWATCHDOG -> completion=GPU BEHIND (completion not delivered) | presenting=frames NOT presenting |
  producer=ALIVE (submitting) | polling=guest NOT polling
GEWATCHDOG -> render-gate dword_8242043C=3 -> render+present ENABLED
GEWATCHDOG -> devflags +21516(VdSwap-skip if !=0)=0x0 | +22280&4(gpu-wait)=0 | +10941=0x6 +10943=0x0
GEWATCHDOG -> vblank ctx[4133]=174 | GPU fences read=32 write=32 [drained]
GEWATCHDOG -> frameCounter 0x8308851C 151->159 [ADVANCING (main thread cycles; render skipped after limiter)] | guestTimebase advancing
```

Read it as:
- **ring DRAINED** (rpi==wpi): the CP consumed every submitted packet — the CP is NOT stuck in WAIT_REG_MEM (unlike the desktop CP-deadlock variant). `cpcnt` (CP counter) is *advancing* (e.g. 89→181→245→308 across present# heartbeats).
- **submit=139 completed=0**: the guest submitted 139 frames' worth but the completion counter (`idblk+0`) is stuck at 0 → "GPU BEHIND (completion not delivered)".
- **skipbit=2 set** (`*(dev+10941)&2`): the frame loop's GPU-completion gate skips render. (Partly set by the ~80ms fallback in `ge_dbg_now`, see below — but the game also manages this bit itself.)
- **render-gate ENABLED** (`dword_8242043C & 2`): rendering is NOT disabled by the game; the skip is purely the completion gate.
- **frameCounter ADVANCING**: guest main thread is alive and looping; only *render* is skipped.

The watchdog's auto-recovery (`ST32(idblk, 0)`, "released CP semaphore") **fires but is ineffective here** — the ring is already drained, so releasing the WAIT_REG_MEM semaphore does nothing. The missing piece is the *completion counter* (`idblk+0`), which the recovery deliberately will NOT write (writing it non-zero historically *held* the CP and caused the original desktop freeze — see the long comment at `ge_dbg_now`).

---

## Key code map

All paths relative to repo root `/home/keith/Projects/GoldenEye-Recomp` and the SDK `../GoldenEye-Recomp-rexglue`.

- `src/ge_hooks.cpp`
  - `ge_dbg_now(r9, r30)` @ **468** — the guest GPU-completion poll hook (guest `sub_82198C28`). Computes `drawn` via (a) CP swap counter moved or (b) ring caught up; (c) ~80ms wall-clock fallback that SETS the skip bit `base[dev+10941]|=2` (line **541**) to force the guest past the wait. **The arch-gated wait is at lines 559-564:** `#if defined(__aarch64__) rex_ge_cp_wait_progress(cp_seq, 2000u); #else std::this_thread::yield(); #endif`. `cp_seq = rex_ge_cp_progress_seq()` @ 486.
  - `ge_dbg_now` also (in the `drawn` path, ~570-579) writes `dev+16552 := dev+16544` (presented := submit) and `idblk+60 := rpi` (RPTR writeback), gated on `dev && idblk && drawn`. It explicitly does **NOT** write `idblk+0` (completion) — see the warning comment ~571-576.
  - `ge_diag_vdswap(r31, r30)` @ **591** — present hook at the guest VdSwap path (`sub_821996F8 @ 0x82199948`). Samples CP counter into `g_present_cpcnt`. Emits the `GEGPU present#N` heartbeat @ **607** (un-gated this session; the Android boot loader greps it — see below).
  - `ge_watchdog_thread()` @ **197** — 250ms poller; detects "present advancing but ring not moving", auto-recovers (`ST32(idblk,0)` @ 232), logs the STALL block @ 244. Started by `ge_start_watchdog_once()` @ 443, invoked from `ge_dbg_now` @ 482. Whole thread is gated by `REXCVAR_GET(ge_freeze_diag)` (default **false**, defined @ 45, category "GPU").
  - Device-offset cheat-sheet (all via `dev`/`idblk` seen by `ge_dbg_now`):
    - `dev+16544` = submit counter
    - `dev+16552` = presented counter
    - `dev+10908` = target
    - `idblk+0`   = **completion counter (the one stuck at 0)**
    - `dev+10941 & 2` = skip bit (`sub_82198C28` returns 0/proceed when set)
    - `0x8242043C & 2` = render gate (set by `sub_8209E1D0`; mode 3=enabled, mode 1=disabled)
    - `0x8308851C` = guest frame counter
- `../GoldenEye-Recomp-rexglue/src/graphics/command_processor.cpp`
  - `rex_ge_cp_wait_progress(last_seq, timeout_us)` @ **156**, and `ge_cp_signal_progress` / `rex_ge_cp_progress_seq` (the CP-worker progress handshake). This is what the arm64 path blocks on. The CP worker bumps the seq on each drain.
- Commits: `90755ad` (introduced the blocking wait + gated freeze diag), `9dd0258` (arch-gated it: desktop yield, arm64 blocking wait). `git show 9dd0258` has the full rationale.

---

## What's been tried / ruled out this session

- **Audio is unrelated to this freeze** — separate subsystem; audio works (see "Session changes"). Don't chase audio for this bug, BUT note the audio fix adds real CPU load on the low-core handheld and *may* make the race lose more often (unproven — worth A/B testing by temporarily forcing `audio_mute` or reverting the audio self-heal).
- **Watchdog CP-semaphore recovery (`idblk:=0`) is ineffective** here (ring already drained; the problem is the completion counter, not WAIT_REG_MEM).
- **The ~80ms skip-bit fallback in `ge_dbg_now` fires** (skipbit=2) and lets the guest *proceed* past the wait, but the frame still renders nothing because `completed` never catches up — so it's a permanent skipped-render loop, not a hard hang.
- **Desktop fix (busy-spin) is NOT directly portable to arm64**: `90755ad`/`9dd0258` say plain yield starves the CP worker on few-core handhelds → a *different* freeze. So "just use yield on arm64 too" likely trades this freeze for the starvation freeze. Verify before relying on it.

---

## Hypotheses / suggested next steps (root cause = completion delivery on arm64)

1. **Why does `idblk+0` (completion) never advance on arm64 while the ring drains?** Trace who is supposed to write the completion counter. On desktop it advances; on arm64 it doesn't. Likely candidates: the CP worker's swap/fence path in `command_processor.cpp`, or a guest-visible fence write that the arm64 blocking-wait path skips. Instrument `rex_ge_cp_wait_progress` and the CP drain/swap to log when (if ever) the completion fence is written on arm64 vs the seq it wakes on.
2. **Is it a lost-signal race like the audio bug?** The audio fix this session was exactly "a 360 single-core hand-off loses a wakeup under true parallelism." The GPU completion path may have the same shape: the CP worker signals progress (seq) but the *completion counter write* that the guest's `(a)`/`(b)` conditions check races with the guest's sample. `drawn` condition (b) `rpi==wpi` should cover the drained case — verify it's actually evaluating true on arm64 (it apparently isn't clearing the guest's own gate). Check whether `cpc != g_present_cpcnt` (a) and `rpi==wpi` (b) are both observed false at the moment of freeze.
3. **Make the watchdog recovery actually recover arm64**: instead of `idblk:=0`, force `completed`/`idblk+0` (or `dev+16552 presented`) to `submit` when stalled AND ring is drained — i.e., synthesize the missing completion. Risky (could present incomplete frames) but the ring is drained so the work *is* done; this may be safe. Prototype behind `ge_freeze_diag` and watch whether the game advances past Twycross.
4. **Tune the arm64 wait**: `rex_ge_cp_wait_progress(cp_seq, 2000u)` — confirm `cp_seq` snapshot vs the CP worker's seq isn't off-by-one (a missed wake leaves the guest blocked until the 2ms timeout, every frame → effective busy-loop that never sees completion). Check `ge_cp_signal_progress` ordering vs the completion-counter write.
5. **A/B the audio-load aggravation**: rebuild with audio effectively disabled (e.g. force the producer to no-op, or `audio_mute`) and measure freeze frequency over ~10 boots to see if the audio CPU load is tipping the race.

The pragmatic fallback the user did NOT pick (but is cheap): fix the boot loader to require *real* frame progress (presented/completion advancing) instead of just `present#`, so frozen boots relaunch and re-roll the ~50% race. See "present# interaction" below.

---

## Session changes already in the working trees (NOT committed)

`../GoldenEye-Recomp-rexglue` (audio — DONE, verified Linux+Android):
- `src/kernel/xboxkrnl/xboxkrnl_threading.cpp` — audio producer (`sub_82366628`) self-heal is now *poll-and-suppress* (`xeKeWaitForSingleObject`): only wakes the guest on a real request (`GeAudioRequestPending()` reads guest AO@`0x8308EC34`, token at AO+300). Fixes total silence; recovers a lost wakeup without the spurious-wake thread-exit. GPU worker `0x821A4A68` keeps its plain −12ms cap.
- `include/rex/audio/conversion.h` — fixed ARM back-L/R downmix channel swap.

`/home/keith/Projects/GoldenEye-Recomp` (boot loader):
- `src/ge_hooks.cpp` @ ~601-607 — **un-gated the `GEGPU present#N` heartbeat** (removed the `&& REXCVAR_GET(ge_freeze_diag)` so it always logs). The Android Java loader (`android/app/src/main/java/com/sunjaycy/goldeneye/GoldenEyeActivity.java` → `hasStartedPresenting()`) greps `ge.log` for `present#N` (N≥`PRESENT_THRESHOLD`=65) to dismiss the "Loading GoldenEye (retry N)" overlay. **present# interaction / caveat:** present# advances even on a *frozen* boot (the VdSwap hook still fires), so the loader now treats a frozen boot as "presenting" and stops relaunching — it lost the accidental re-roll-until-good behavior it had when present# was never emitted. If you fix the freeze this is moot; if not, consider gating present# on *real* presentation (e.g. only emit when `drawn`/presented advanced).

**Device state:** the APK currently installed on the Ayn Thor was built with a TEMP `ge_freeze_diag=true` line in `src/ge_app.h::OnConfigurePaths` that has since been **reverted in source**. Rebuild+reinstall to get a clean build, or re-add it for diagnostics.

---

## Build / run / debug recipe

### Android (Ayn Thor, the relevant platform)
- Build+install (the gradle externalNativeBuild recompiles the rexglue SDK + recomp for arm64 and bundles `libge.so`+`librexruntimerd.so`; ccache makes incremental ~seconds):
  ```
  cd /home/keith/Projects/GoldenEye-Recomp/android
  ./gradlew :app:installDebug -PrexSdkDir=/home/keith/Projects/GoldenEye-Recomp-rexglue
  ```
  **Gotcha:** `gradle.properties` `rexSdkDir=../../GoldenEye-Recomp-rexglue` resolves relative to the *app module* dir → wrong path → CMake configure fails. ALWAYS pass `-PrexSdkDir=<abs path>`.
  **Install downgrade:** if it fails `INSTALL_FAILED_VERSION_DOWNGRADE` (repo is versionCode 1; device may have newer), run `adb install -r -d app/build/outputs/apk/debug/app-debug.apk`.
- Launch: `adb shell monkey -p com.sunjaycy.goldeneye -c android.intent.category.LAUNCHER 1`
- Logs/state on device: `/sdcard/Android/data/com.sunjaycy.goldeneye/files/` → `ge.log` (rexglue log, info level), `stderr.txt` (guest GE* hook traces incl. GEBOOT/GEWATCH/GETEAR), `default.xex`, music/sfx banks, `cache/`.
  ```
  adb shell "cat /sdcard/Android/data/com.sunjaycy.goldeneye/files/ge.log" | grep -E "GEWATCHDOG|GEGPU present#"
  ```
- Screenshot (to see if it's past the Twycross frame): `adb exec-out screencap -p > /tmp/s.png`
- Crash backtraces: device writes **proto** tombstones (logcat crash buffer is empty). Use `adb bugreport /tmp/bug` and read rendered text tombstones in the zip under `FS/data/tombstones/tombstone_NN` (the non-`.pb` files), filtered for `Cmdline: com.sunjaycy.goldeneye`.

### Enabling diagnostics on Android (no config file is read!)
On Android the config path resolves under read-only `/system/bin/ge.toml` and is never loaded; argv is synthesized in `../GoldenEye-Recomp-rexglue/src/ui/windowed_app_main_android.cpp` (only sets `--game_data_root/--log_file/--cache_path/--user_data_root`). So to set a cvar like `ge_freeze_diag`, add a line in `GeApp::OnConfigurePaths` (`src/ge_app.h`):
```cpp
rex::cvar::SetFlagByName("ge_freeze_diag", "true");
```
then rebuild+install. That starts the freeze watchdog (250ms poller, auto-recovery, the STALL diagnostics above).

### Linux (for comparison / fast iteration; this platform is NOT frozen — it boots past Twycross)
```
cd /home/keith/Projects/GoldenEye-Recomp
cmake --build --preset linux-amd64-relwithdebinfo --target ge
export LD_LIBRARY_PATH=/home/keith/Projects/GoldenEye-Recomp-rexglue/out/linux-amd64:$LD_LIBRARY_PATH
./out/build/linux-amd64-relwithdebinfo/ge --game_data_root=$PWD/assets --log_level debug
```
The rexglue SDK builds as a subproject of the game build (REXSDK_DIR via add_subdirectory): editing `../GoldenEye-Recomp-rexglue` source and rebuilding the `ge` target recompiles+relinks `librexruntimerd.so` automatically. Display + PipeWire are available on the dev box (Fedora, AMD Radeon 890M/RADV, Wayland). Cvars are CLI flags on desktop (`--ge_freeze_diag`, `--log_level debug`).

### Gotchas observed
- `pkill -f "/ge ..."` matches the killing shell's own command line → self-kill. Use `pkill -x ge` / `pkill -9 -x ge`.
- Backgrounding `./ge &` inside a one-shot Bash tool gets torn down when the call returns; run it as a tracked background task instead.
- `log_verbose` cvar is `.debug_only()` and may be absent in relwithdebinfo; use `--log_level debug`.

---

## Project memory (already written, for continuity)
- `goldeneye-audio-silence-fix` — the audio root cause/fix.
- `goldeneye-android-boot-loader-present-marker` — the present#/loader coupling + Ayn Thor build gotchas.
- `goldeneye-first-screen-freeze` — the original (desktop) freeze diagnosis.
- `goldeneye-android-port-state` — "~50% boots black (GPU-ring race)".
