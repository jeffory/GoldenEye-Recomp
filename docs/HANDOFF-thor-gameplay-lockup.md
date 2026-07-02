# HANDOFF: Thor gameplay lockup — infinite write-watch fault loop (OPEN)

**Status 2026-07-02:** two live-diagnosed lockups, one contributing bug fixed (maps parsing),
**root cause still open**: a guest write that faults on a watched page is never resolved by the
write-watch machinery and re-faults forever. Everything below is reproducible-from-evidence; the
raw captures live in `out/lockup-2026-07-02-b/` (second, decisive capture) and the session
scratchpad of 2026-07-02 (first capture).

## The bug in one paragraph

~20+ minutes into a real gameplay session, one guest thread (the frame-loop thread — the same tid
that emits GEFPS lines) enters an infinite SIGSEGV loop: guest fn `sub_82123DB8` (via
`__imp__sub_821448F8`, 97% of sampled stacks) performs a write to a write-watched host page →
SIGSEGV → `MMIOHandler::ExceptionCallback` → `QueryProtect` says the page is still protected →
`access_violation_callback_` (PhysicalHeap::TriggerCallbacks path, `xmemory.cpp`) returns
*handled* → the faulting instruction retries → **faults again**. The watch for that address class
is never actually lifted. The game stops presenting entirely (present# and submit both freeze);
every other thread parks. In lockup #1 each fault additionally re-parsed all of /proc/self/maps
(fixed, see below) which made the loop ~100x slower but the loop itself is the disease.

## Evidence

`out/lockup-2026-07-02-b/`:
- `ge.log` — full session incl. the new `GEWATCHDOG TOTAL-FREEZE` dump (fired ~3s after freeze):
  `submit=40543 presented=40544 present#=85930 | ring rpi==wpi [DRAINED] | render-gate=3 |
  skipbit-fires=0 wrm-timeouts=0 starved=1`, last-frame stages
  `cpexec=18.3ms wrm=14.9ms present=35.6ms gwait=0 gpu=17.3ms`.
- `leadup-trail.txt` — the death spiral: paint block degrades 4.8→36ms, shown/s 60→27.6,
  drop/s 0→22.6 over the last minute (same degradation shape as lockup #1: escalating GESPIKEs
  with big `present=`, `starved=1`).
- `threads.txt` — Thread-5 (tid 14941) state R with utime=338950 stime=493987 (≈83min CPU);
  ALL other threads S (CP worker, VSync, UI thread all parked).
- `wedge2.perf.data` + `spinner-*.txt` — simpleperf of the wedge; symbolize with
  `--symfs symfs/` (unstripped libs already staged there). Top of stack: art SignalChain /
  sigprocmask / kernel sigreturn ≈50%, `MMIOHandler::ExceptionCallback` 69% of handler time,
  `QueryProtect` 44.8% (now O(1) shadow hits — mutex+hash per fault at storm rate).
- `logcat.txt` — no kernel GPU faults; note Thor's `pservice` periodically clamps kgsl
  min_pwrlevel (suspected contributor to the paint-block degradation, unproven).

## What was already fixed (don't re-do)

1. **SDK `3496bf3` fix(memory):** fault path no longer parses /proc/self/maps —
   `FindEntryForAddress` rewritten async-signal-safe (raw syscalls, no ifstream/std::string in
   the SIGSEGV handler); `QueryProtect` memoizes fallback results into `g_prot_shadow`;
   `AllocFixed` populates the shadow (≤4MB ranges) — it never did, which was the original
   shadow-miss class. Verified live: lockup #2's profile shows no maps parsing.
2. **Game `f4b1227` GEWATCHDOG TOTAL-FREEZE trigger:** fires when submit+present# both stagnate
   ~3s (the old STALL trigger needs presents alive and stayed silent through lockup #1). Fired
   correctly in lockup #2, zero false positives in desktop/Thor soaks.

## Root-cause hypotheses for the new session (ordered)

1. **TriggerCallbacks doesn't lift the protection for the faulting address.**
   `PhysicalHeap::TriggerCallbacks` (`GoldenEye-Recomp-rexglue/src/system/xmemory.cpp:~564+`,
   unprotect path ~:528) computes the unprotect range from the *guest virtual* address; if the
   fault host address maps to a guest page whose `page_table_` state disagrees (e.g. the
   watch bit was already cleared but mprotect never happened, or a different mapping view than
   `TranslateRelative` targets), it returns handled without changing the faulting page.
   Instrument: in `MMIOHandler::ExceptionCallback`'s !range path, count consecutive faults on
   the SAME fault_host_address; at N=1000 log address, guest PC (`ex->pc()`), and the
   page_table_ state, then consider a failsafe hard-unprotect (mprotect RW + log) to turn the
   freeze into a one-frame artifact.
2. **Watch re-armed instantly by another thread** (shared_memory re-watching the range between
   unprotect and retry). The global_critical_region lock is held through the callback, so this
   needs the re-arm to happen after release — check `EnableAccessCallbacks` callers.
3. **The `cur_access` early-abort path** (`mmio_handler.cpp:427`): if the shadow ever went stale
   claiming writable while the real page is read-only, the handler returns true WITHOUT
   unprotecting → same-instruction loop. rex::memory is the only mprotect caller in-tree
   (verified by grep), but audit `ShadowSet` coverage vs. every mprotect site again if (1)
   dead-ends. Lockup #1 predates the memoization (QueryProtect then read truth from maps) and
   STILL looped — so (1)/(2) are more likely than shadow staleness.

Also worth attention: `sub_82123DB8` / `__imp__sub_821448F8` — identify what this guest routine
writes (framebuffer? animation buffer?) from the recomp sources in `generated/` — the write
target class may make hypothesis (1) obvious.

## Secondary lead (separate issue)

Both lockups were preceded by ~1min of present-path degradation (paint 4.8→16→36ms, drop/s
rising) — possibly Thor thermal/power (pservice kgsl clamping) or swapchain backpressure. The
GESHOWN line tracks it. If gameplay stutters build over minutes without a freeze, that's this.

## Recipes

- Build+deploy Thor: `cd android && JAVA_HOME=~/.gradle/jdks/eclipse_adoptium-17-amd64-linux.2
  ./gradlew :app:installDebug -PrexSdkDir=/home/keith/Projects/GoldenEye-Recomp-rexglue`
- Capture a live wedge (no root):
  1. `adb pull /sdcard/Android/data/com.sunjaycy.goldeneye/files/ge.log` (grep TOTAL-FREEZE)
  2. thread states: `/proc/<pid>/task/*/stat` (R + huge utime/stime = spinner)
  3. `adb shell run-as com.sunjaycy.goldeneye simpleperf record -g -p <pid> --duration 5 -o
     /data/data/com.sunjaycy.goldeneye/w.perf.data`, cat it out via run-as
  4. symbolize on host: NDK simpleperf `report --symfs <dir>` with
     `android/app/build/intermediates/merged_native_libs/debug/.../lib/arm64-v8a/*.so` staged
     under the APK's device path.
- Desktop repro attempt: same SDK code runs on Linux; the fault storm class may reproduce with
  long sessions (`--ge_fps_log=true --ge_spike_log=true`, watch for TOTAL-FREEZE).

## State of the world

- Repos: game `perf/arm64-handheld` == `develop` == origin (f4b1227); SDK `main` == origin
  (3496bf3). Full perf-diagnostics stack landed today (GEFPS/GESHOWN/GESPIKE lines, per-frame
  CSV + `scripts/perf_report.py`, overlay stage bars, Vulkan GPU timestamps) — see
  `goldeneye-perf-diagnostics-stack` memory and the ge_fps.cpp header comment for semantics.
- The Thor build currently installed contains all of the above.
