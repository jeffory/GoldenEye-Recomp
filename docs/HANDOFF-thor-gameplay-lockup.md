# HANDOFF: Thor gameplay lockup — ROOT CAUSE CONFIRMED (failsafe absorbs it)

**Status 2026-07-02 (final): root cause captured live and symbolized.** During a 14-min Thor
session the failsafe fired at 15:23:37 (`out/session-2026-07-02-c/ge.log` line 4173) and the
ring recorded the entire loop: **64 back-to-back writes (~2µs apart) to host
0x100000010 = guest virtual 0x10 — the protected zero page.** PC symbolizes to
`__imp__sub_821448F8` (ge_recomp.5.cpp:62918), the effect-spawn "flag byte @+16, xyz floats
@+20/24/28" write — through a **NULL object pointer** (0x10 = NULL+16). Chain: heavy combat
exhausts the 200×32B effect pool (`sub_82144058`, pool at guest 0x82064E00) → allocator
returns 0 → `sub_82123DB8`→`sub_821448F8` writes NULL+16 unchecked → `protect_zero` keeps
guest page 0 NoAccess → `Memory::AccessViolationCallback` declines (v00000000 heap is not
kGuestPhysical) → **unhandled SIGSEGV silently retries the instruction** (ExceptionHandler
falls through) → infinite loop. That's the "lockup while shooting an enemy". All prior
write-watch hypotheses (veto/re-arm/stale shadow) were bystanders — though the stale-shadow
infinite loop found+fixed en route (test scenario 5) was real and could have caused lockup #2.

After the failsafe unprotected page 0 the game ran clean for 6+ more minutes (user-confirmed
"crashes seem fixed"). Residual cost: one 64-fault burst (~130µs) per first exhaustion, then
zero — page 0 stays RW for the session.

**Remaining follow-ups (all optional polish):**
1. Ring-dump race: the watchdog dumps up to 250ms after the failsafe; healthy traffic
   (~430 rec/s) can wrap the 128-slot ring. Fix: freeze/snapshot the ring in the failsafe
   itself, or dump from the raw stderr trigger. (Today's capture survived only because the
   loop records were dense.)
2. Real fix candidates for the NULL write (beyond the failsafe): investigate WHY the pool
   exhausts (leak? effects stop spawning once full = visual bug even without the crash);
   or a midasm guard on sub_82123DB8 skipping the attach when the allocator returns 0;
   or `protect_zero=false` on Android (matches post-failsafe state without the burst).
3. Unhandled-fault silent retry in ExceptionHandlerCallback is a footgun — consider a loud
   log + abort after N retries at that layer too (the diag `cb_unhandled` counter now at
   least makes it visible).

**Separate issue confirmed the same session: Dam displayed-fps is GPU-bound, not a lockup.**
Dam minutes 15:20-15:25: `shown/s` collapses to ~26-29 with `drop/s`≈23 and `paint`≈35-39ms
while the guest simulates ~50fps (`refresh/s`≈50) — spike-frame `gpu=`≈20ms > 16.6ms vsync
budget → FIFO present quantizes the display to ~half refresh while the guest keeps producing
(so guest-side FPS meters read ~51: the "incorrectly measured" feel). Facility: gpu 12-14ms →
shown/s 60, everything agrees. wwf= on spike frames is 4-7 (≈0.3ms) — faults do NOT cause the
spikes.

**Render-scale A/B result (2026-07-02 16:08, resolution_scale=2 baked at boot):** Dam at 2x
(4x pixels): gpu 39.5ms, shown/s 12.4, guest refresh 25/s, paint 80ms; GPU clock boosted to
its 680MHz max (no thermal clamping observed; temp hit 86°C at 2x). Solving F+P=20 /
F+4P=39.5: **P≈6.5ms pixel-rate work, F≈13.5ms resolution-independent GPU work at 1x — Dam is
~2/3 fixed-cost (geometry/binning/per-draw), only ~1/3 fill.** 1x is already the resolution
floor (resolution_scale is an integer upscale, min 1), so resolution can't buy Dam back.
Realistic directions: (a) frame-pacing quality — a 30fps cap or mailbox present turns the
26-29+23drops/s oscillation into steady pacing; (b) attack the 13.5ms fixed GPU cost in the
SDK Vulkan backend (draw batching / geometry path) — large effort; (c) nothing — Facility-class
levels already run 60. protect from regressions with GESHOWN wwf/paint columns.

**New bug found during the experiment:** the overlay VIDEO-tab Render Scale quick-restart
latches the 80ms skip-bit fallback on the Thor — GEWATCHDOG STALL "SKIP-BIT LATCHED / frames
NOT presenting" at 15:48:19, render permanently skipped until app restart. The June cold-boot
fix (CP worker priority) does not cover the quick-restart re-init window. Workaround: bake
cvars at boot. Fix idea: re-apply the boot-time skip-bit clear / rendered# gating on the
quick-restart path too.

--- Previous session status (mechanism reference) ---

**Status 2026-07-02 (evening session): instrumented + failsafed + one loop class root-fixed;
awaiting a long device session for the verdict.** What landed (SDK uncommitted work tree +
game repo, deployed to the Thor):

- **A reproducible infinite-loop class found and fixed (S2, stale shadow).** New SDK test
  `tests/unit/memory/write_watch_fault_test.cpp` scenario 5 makes the protection shadow claim
  RW while the kernel has the page RO: the old `ExceptionCallback` early-abort then returns
  handled forever without touching anything — a genuine wedge, verified by stashing the fix
  (test times out) and restoring it (passes). This signature matches lockup #2's profile
  (all handler time in QueryProtect, TriggerCallbacks absent from stacks), and the shadow
  memoization (3496bf3) landed *between* lockups #1 and #2. Fix: at 16+ consecutive
  same-page faults the handler re-queries kernel truth via the new async-signal-safe
  `rex::memory::QueryProtectUncached` and overrides a lying shadow (mmio_handler.cpp).
- **Failsafe:** ≥64 back-to-back same-page faults (<2ms gaps — frame-paced re-arms don't
  count) → hard mprotect-RW + `REXWWFAILSAFE` raw stderr line + `GEWWFAILSAFE` in ge.log
  with a full diag dump. Any residual loop variant becomes a one-frame artifact.
- **Instrumentation (`rex/system/fault_diag.h`):** counters + 128-record ring across all
  seven decision points (early-abort, callback verdicts, unprotect-veto @ xmemory
  TriggerCallbacks, any_watched, recovery incl. kernel-truth verify, arm-side re-arm race,
  PhysicalHeap::Protect RO-on-watched). Game side: `GEWWDIAG` summary every ~5s when
  counters move + full ring dump on TOTAL-FREEZE, `wwf=` on GESPIKE, `wwf/s=` on GESHOWN.
- **Baselines (healthy):** desktop menus ~65 wwf/s; Thor menus ~70 wwf/s, consec_max 3,
  vetoes 0, shadow_disagree 0. NOTE: `rearm` ticks constantly in normal play (GPU re-arms a
  just-faulted page within 5ms routinely) — re-arm alone is NOT pathology; look for it
  co-occurring with vetoes/streaks.
- Desktop synthesis of the veto (S1/#1 hypothesis) shows it self-recovers in ≤3 faults via
  the AccessViolationCallback recovery path — the veto alone cannot wedge; it needs the
  re-arm race or the stale shadow to sustain. The Thor evidence will name the variant.
- Thor host page size confirmed 4KB (16KB mixed-page theory ruled out).

**Next:** rapid-shooting session (minutes) → read `wwf=` on GESPIKE lines to confirm the
shooting↔fault-storm link; then a 20-30min combat session → either it survives (S2 fix was
the root cause), or GEWWFAILSAFE fires with the ring naming the loop variant → apply the
matching Phase C fix (see plan file / table below). Test infra gotcha: the write-watch tests
are a separate binary (`write_watch_fault_tests`) because Catch2's per-test signal-handler
save/restore strands rex's once-installed SIGSEGV handler; both test binaries also segfault
in static destructors AT EXIT (pre-existing rexcore double-link) — results print fine,
discovery uses PRE_TEST mode.

--- Original handoff below (still-valid mechanism reference) ---

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
