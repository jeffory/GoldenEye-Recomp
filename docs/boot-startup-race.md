# GoldenEye‑Recomp (Android) — Boot Startup Race

Status: **OPEN** (mitigated, not root‑fixed). Last updated 2026‑06‑16.

This document captures everything learned about the intermittent cold‑boot hang on the
Android port so it can be root‑caused and fixed later. It is deliberately detailed: the
bug is timing‑dependent and easy to misdiagnose, and several plausible hypotheses have
already been **disproven** — re‑chasing them wastes a lot of time.

---

## 1. TL;DR

* **Symptom:** ~**42–50 %** of cold launches hang on a black screen; the rest boot to the
  gun‑barrel intro → menus → gameplay and run fine. The outcome is decided very early,
  before the first presented frame.
* **It is a per‑process race** in the *guest's own* startup: the main game thread can wedge
  spinning in its frame‑limiter / GPU‑completion wait (`sub_82198C28`, polling the guest
  clock) before the first frame. A fresh relaunch is an independent ~42 % roll.
* **Currently mitigated by auto‑retry** in the Java shell (`GoldenEyeActivity.java`): a
  watchdog relaunches a stalled boot until one wins the race (~99.6 % within 10 attempts),
  with a loading spinner. **This makes the game reliably bootable but does not fix the
  underlying race.**
* **Root cause is characterized but not fixed.** The guest frame‑limiter spin is the same
  *class* of "single‑core‑era lock‑free hand‑off that loses under true multi‑core
  parallelism" bug that already has two hand‑written guards in the runtime.

---

## 2. What it is NOT (disproven — do not re‑chase)

| Hypothesis | Verdict | Evidence |
|---|---|---|
| Missing / wrong game assets | **NO** | Desktop renders with the exact same files. The `loc\english\…` / `(null)` `entry not found` warnings appear on **successful** boots too (≈300/boot) — benign optional‑loc probes. |
| GPU‑register `CP_RB_WPTR` protection race ("mode A") | **NO** | A vsync‑thread watchdog polling `QueryProtect(host(0x7FC80000))` showed the GPU register page stays `PROT_NONE` on every boot (`protLost=0`); on good boots `CP_RB_WPTR` dispatches 100k+ times. |
| Audio get‑or‑create churn / teardown | **NO** | One‑shot `GEAUDREG`/`GEAUDUNREG` on the kernel `XAudioRegister/UnregisterRenderDriverClient` entries: on a stall, **`XAudioRegisterRenderDriverClient` returns `0x0` and is never unregistered** — no churn. (The original audio churn bug *was* real and is fixed; the audio worker `sub_82366628` infinite‑wait seen earlier was a *symptom* of the guest being stuck, not the cause.) |
| Guest memory corruption | **NO** | The "mangled paths" (`game:iles\locnglish`) were a terminal artifact — `\f`/`\e` rendered as control chars. `cat -v` shows the real path is clean (`game:\files\loc\english\…`). |
| Graphics init failure | **NO** | Vulkan/Adreno init, 2400×1080 mailbox swapchain, GPU/runtime/shader‑storage all come up **cleanly and identically** on good and bad boots (swapchain ~2 s after launch). |

---

## 3. What it IS (characterized root cause)

On a **stalled** boot the guest **main game thread wedges spinning in the frame‑limiter /
GPU‑completion wait**, before the first present.

### 3.1 Thread states on a stall (zero‑perturbation, via `run-as … cat /proc/<tid>/wchan`)

```
Audio Worker (F8…)   wchan=0                     (running — NOT the gate)
GPU Commands (F8…)   wchan=futex_wait_queue_me   (idle, waiting for ring commands — normal)
Main XThread (F8…)   wchan=0                     (running)
Main XThread (F8…)   wchan=__arm64_sys_nanosleep (POLLING)
Main XThread (F8…)   wchan=__arm64_sys_nanosleep (POLLING)
Vulkan Pipeline ×6   wchan=futex_wait            (idle)
```

The main game threads are **nanosleep‑polling**, *not* blocked on a futex — so this is **not
a simple lost‑wakeup on a guest kernel object** (`KeWaitForSingleObject`/`Multiple`); an
infinite‑wait tracer confirmed no main‑thread infinite guest kernel wait.

### 3.2 simpleperf of a stall (`--app`, software `cpu-clock`; debuggerd needs root)

Hot stack on a **stall** (absent on a **good** boot — this is the discriminator):

```
XThread::Execute → sub_821898D0 → sub_821996F8 → sub_8219D060 → __imp__sub_82198C28
    → rex::chrono::Clock::QueryGuestTickCount()      (~82 %)
    → rex::chrono::Clock::QueryGuestUptimeMillis()   (~53 %)
  + disruptorplus::spin_wait_strategy::wait_until_published   (timer‑queue idle‑spin)
  + PosixConditionBase::WaitMultiple (poll) + clock_gettime + 128‑bit atomics
```

The timer‑queue disruptor spin and the audio‑worker `WaitMultiple` poll are **symptoms**
(infrastructure idle‑spinning because the guest stopped arming timers / never pumps audio).
The **gate** is the guest spinning in `sub_82198C28` reading the guest clock.

### 3.3 The frame‑limiter / GPU‑completion wait (`sub_82198C28`)

Decoded at `generated/ge_recomp.8.cpp:36055‑36090`. Loop body (r29 = device struct):

```
loc (spin top):
  if ( *(device+10941) & 0x2 )  goto proceed;       // the "skip bit" — bypasses the wait
  r9  = *(r13+256);
  r10 = *(r31+8);                                    // submitted counter
  r30 = *(r9+88);                                    // the "now" time field  (= *(r9+0x58))
  ge_dbg_now(r9, r30);                               // HOOK: feeds the now field (see below)
  r9  = *( *(device+10896) );                        // completed counter
  if ( r10 == r9 ) goto proceed;                     // GPU caught up -> done
  ... loop ...
```

So the wait is: spin until **(a)** the skip bit `device+10941 & 0x2` is set, OR **(b)**
`submitted == completed` (the GPU finished the frame). `__imp__sub_82198C28` is hooked
(`ge_dbg_now`) which reads the guest timebase (`QueryGuestTickCount` shows up here).

### 3.4 The `now` field and `ge_dbg_now` (`src/ge_hooks.cpp`)

* `*(r9+0x58)` (the `now` field) is "a hardware/kernel time the game only READS; rexglue
  never ticks it -> frozen at 0 -> infinite spin." `ge_dbg_now` (mid‑asm hook wired at
  `ge_recomp.8.cpp:36070`) **feeds it the real `REX_QUERY_TIMEBASE()`** each iteration so the
  `(now‑last)` time check advances.
* `ge_dbg_now` also has an **in‑hook 80 ms watchdog** that **sets the skip bit**
  (`device+10941 |= 0x2`) when the GPU‑completion wait stalls, so `sub_82198C28` returns and
  the guest proceeds. **This is the established recovery for the post‑first‑frame freeze.**

### 3.5 Why it's a race (single‑core‑era assumption)

* `src/core/clock.cpp:71‑81` documents the lineage: GoldenEye's frame‑limiter is a tight
  spin that reads the timebase millions of times/sec. The *original* failure was a
  **mutex‑holding stall** in the clock‑advance path (a preempted holder froze the clock for
  all threads → spin never reached its target → freeze). That was fixed by making the guest
  clock a **lock‑free pure function of the host clock** (`UpdateGuestClock`).
* GoldenEye has **several** such lock‑free hand‑offs written for the 360's single core that
  lose wakeups under true multi‑core parallelism. Two already have hand‑written guards in
  `src/kernel/xboxkrnl/xboxkrnl_threading.cpp` (the `xeKeWaitForSingleObject` deadlock guard
  caps infinite waits to ~30 ms for these start addresses):
  * `sub_821A4A68` — deferred GPU command worker (ring drains → screen freeze).
  * `sub_82366628` — audio client worker (added this session; removed its infinite wait but
    did **not** fix the boot, confirming it's downstream).
* **The boot‑time frame‑limiter spin is another manifestation of this class, not yet
  guarded.** It is NOT covered by `gpu_stall_recovery.h` (that only protects events the GPU
  ISR signals — render/vblank waits) nor by `ge_watchdog_thread` (that only acts on the
  *post‑render* freeze, gated on `present_alive`).

---

## 4. Attempted fixes that did NOT work (so they aren't re‑tried)

1. **Cap `sub_82366628` (audio worker) infinite wait** → removed the wait, boot still stalls
   (audio is downstream).
2. **Boot‑recovery in `ge_watchdog_thread`** (when `present==0` + guest spinning, release the
   CP semaphore `idblk:=0` + set the skip bit `device+10941|=2`): **never fired**, because
   (a) `ge_watchdog_thread` is started *lazily from inside `ge_dbg_now`*, and (b) the in‑hook
   80 ms watchdog **already sets the skip bit** on a stall and it **does not unblock the
   boot** — so setting the skip bit is not the boot fix. Reverted.
   * Implication: the boot stall is **not** broken by the skip bit, OR the boot spin is in a
     code path that doesn't reach `ge_dbg_now`'s 80 ms watchdog. Worth confirming whether
     `g_dbgnow_calls` advances during a *boot* stall (does `ge_dbg_now` even run pre‑first‑frame?).

---

## 5. Current mitigation — auto‑retry + loading spinner (`GoldenEyeActivity.java`)

* A **watchdog on a dedicated thread** (the Java main Looper *wedges* with the guest via
  `android_native_app_glue`'s synchronous command handshake, so a main‑thread `Handler` is
  unusable) polls `ge.log` for `GEGPU present#N` with **N≥65** (≥64 frames = a live render
  loop; a wedged boot can emit a lone `present#1` then freeze, so any‑present# is not enough).
* If no live render within **16 s**, it `startActivity(NEW_TASK|CLEAR_TASK)` (foreground
  launch, not background‑blocked) + `Runtime.exit(0)` to kill the wedged process and re‑roll,
  up to **10 attempts**. `ge.log` is deleted in `onCreate` *before* `super.onCreate`.
* A **loading overlay** (separate `APPLICATION_PANEL` window — NativeActivity *takes* the game
  window surface so a normal View can't be used; managed entirely on its own thread so it
  animates even while the main thread is wedged) shows a spinner + "Loading GoldenEye…" +
  "(retry N)", removed when a live render is confirmed.
* **Verified:** S20 FE (Adreno 650) 6/6 cold launches boot to a live render (8–40 s, 0–2
  retries each); Ayn Thor (Adreno 740) booted first try.
* ⚠️ **`ge_diag_vdswap` (`GEGPU present#` in `ge_hooks.cpp`) is now LOAD‑BEARING** — the
  auto‑retry detects "rendering" from it. Do **not** strip it as "temp instrumentation."

---

## 6. Methodology lessons (critical — these cost a lot of time)

* **Do NOT instrument per‑guest‑wait or per‑store.** Logging in `KeWaitForSingleObject`/
  `Multiple` or redefining `REX_STORE_U32` to a watch hook **perturbs the very timing race**
  and regressed the boot rate toward **0 %**. Use **one‑shot / post‑frame** instrumentation only.
* **The rapid force‑stop/relaunch loop is biased to fail** (~0–25 % regardless of the true
  rate). Measure with **organic, spaced** single launches; use `ge.log present#` (not
  screencap byte size, which is an unreliable black/content proxy on these devices) as the
  render signal.
* **Thermal throttling changes the rate** — the spin is CPU‑speed‑dependent ("worse at higher
  fps because the spin hammers harder"). Long test runs throttle the device (one 0/12 anomaly
  was pure SKIN‑throttle); reboot/cool between batches. Check `dumpsys thermalservice` +
  `scaling_cur_freq`.
* `ge.log` is **append** mode → `rm` it before each run, or stale `present#` confuses detection.
* Tooling on a non‑rooted but **debuggable** APK: `debuggerd -b` needs root (unusable);
  `simpleperf record --app <pkg> -e cpu-clock -g` works; `/proc/<pid>/task/<tid>/{comm,wchan,
  syscall}` are readable via `run-as <pkg> …`.
* `present#` is logged every 64 frames (`present#1, 65, 129, …`). A wedged boot can emit a
  lone `present#1`.

---

## 7. Concrete next steps to root‑cause

The auto‑retry makes this non‑urgent, but to cut boot latency / actually fix the race:

1. **Determine which branch of `sub_82198C28` the boot spins on** (one‑shot, low‑overhead):
   is it waiting on the **time field** (`now = *(r9+0x58)` not advancing) or the **GPU‑
   completion counter** (`*(device+10896)` never catching `*(r31+8)`)? Log both once when a
   boot has spun >3 s with `present==0`. Compare a good vs bad boot at the divergence point.
2. **Does `ge_dbg_now` even run pre‑first‑frame on a bad boot?** Check whether
   `g_dbgnow_calls` advances during a *boot* stall. If not, the boot spin is a *different*
   loop than the post‑render frame‑limiter, and the `now`‑field feeding / 80 ms skip‑bit
   never engages → that's the gap to fill (e.g. start `ge_watchdog_thread` **unconditionally**
   and add a boot‑recovery that doesn't depend on `dbgnow`).
3. **Is it the CPU↔GPU semaphore deadlock before the first frame?** The post‑render watchdog
   recovers the in‑game freeze by writing `idblk:=0` (release the CP's `WAIT_REG_MEM`). The
   boot version of that recovery was never actually exercised (watchdog not running at boot).
   Start the watchdog early and try releasing `idblk` + setting the skip bit on a
   `present==0` boot stall — but verify `dev`/`idblk` are valid that early.
4. **Guest‑thread startup ordering.** Multiple "Main XThread" instances race at boot (some
   nanosleep‑polling, some running). Check whether the main thread polls a flag/counter that
   another guest worker is supposed to set, and whether that worker is itself wedged — i.e. a
   guest‑internal startup barrier that races. Also review the AArch64 fiber hand‑off
   (`GoldenEye-Recomp-rexglue/src/core/fiber_android_arm64.S` + `fiber_posix.cpp` Android
   branch) for a startup‑scheduling race.
5. **Compare against desktop.** The desktop build (Linux/Vulkan/GTK) boots reliably. Diff the
   guest execution at the init→first‑frame transition (the desktop reaches "Creating graphics
   pipeline state with VS …" then climbs `present#`; the Android stall stops just before the
   first draw, after loc loading).

---

## 8. File / address reference

**Wrapper (`GoldenEye-Recomp`):**
* `src/ge_hooks.cpp` — `ge_dbg_now` (frame‑wait hook + in‑hook 80 ms skip‑bit watchdog),
  `ge_watchdog_thread` (post‑render freeze recovery), `ge_diag_vdswap` (`GEGPU present#` —
  load‑bearing for auto‑retry), `ge_start_watchdog_once`.
* `generated/ge_recomp.8.cpp:36055‑36090` — `sub_82198C28` frame‑wait; `ge_dbg_now` call at
  `:36070`.
* `android/app/src/main/java/com/sunjaycy/goldeneye/GoldenEyeActivity.java` — auto‑retry
  watchdog + loading overlay.

**SDK fork (`GoldenEye-Recomp-rexglue`):**
* `src/core/clock.cpp` — `UpdateGuestClock`, `QueryGuestTickCount/UptimeMillis`; frame‑limiter
  lineage comment (lines ~71‑81).
* `src/kernel/xboxkrnl/xboxkrnl_threading.cpp` — lost‑wakeup deadlock guard (`xeKeWaitForSingleObject`,
  start addresses `0x821A4A68`, `0x82366628`).
* `src/graphics/graphics_system.cpp` — `DispatchInterruptCallback`, `MarkVblank`, vsync worker,
  GPU MMIO range (`AddVirtualMappedRange(0x7FC80000…)`), `WriteRegister` (CP_RB_WPTR 0x01C5).
* `include/rex/gpu_stall_recovery.h` — the "render event" un‑loseable‑wakeup mechanism (ISR
  KeSetEvent protection).
* `src/kernel/xboxkrnl/xboxkrnl_audio.cpp` — `XAudioRegister/UnregisterRenderDriverClient`
  (+ one‑shot `GEAUDREG`/`GEAUDUNREG` if still present).

**Key guest addresses / fields:**
* Main game thread start: `0x8235E4A8`. Frame‑wait fn: `sub_82198C28` (called via `sub_8219D060`).
* Frame‑limiter PC ranges: `0x823B3040‑0x823B3540`, `0x82189DC0‑0x82189E14`.
* `device+10941` = skip bit (`&0x2`); `device+10896` = completed‑counter ptr;
  `device+16544` = submit; `device+16552` = presented; `r31+8` = submitted counter.
* Frame counter `dword_8308851C` (updated each frame after the limiter); render‑gate
  `dword_8242043C` (`&0x2` = render+present enabled). Audio singleton `0x8308EC34`.

---

## 9. How to reproduce / test

```
# Build SDK runtime, then wrapper APK + deploy + launch:
cd GoldenEye-Recomp-rexglue && cmake --build out/build/android-arm64 -j
export ANDROID_SERIAL=<device>           # Adreno GPU required (Mali lacks vertexPipelineStoresAndAtomics)
/tmp/ge-deploy-java.sh

# Organic reliability measure (NOT a rapid loop), present# from ge.log = render signal:
#   force-stop; sleep 10; rm ge.log; am start …; sleep 24; check max present# (>100 = booted)
# Stall inspection (zero perturbation):
#   run-as <pkg> sh -c 'cd /proc/<pid>/task && for t in [0-9]*; do echo $t "$(cat $t/comm)" $(cat $t/wchan); done'
#   simpleperf record --app <pkg> -e cpu-clock -g -f 2000 --duration 5 -o /data/local/tmp/p.data; simpleperf report -i … -g caller
```

Target hardware: **Adreno (Qualcomm) GPU, arm64‑v8a.** Verified on Galaxy S20 FE 5G
(Adreno 650) and Ayn Thor (Adreno 740). Mali GPUs cannot run the Vulkan backend.
