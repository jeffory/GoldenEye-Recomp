# First-Shot Hitch Attribution — Capture Findings (Phase 2A)

**Date:** 2026-07-11 (Ayn Thor, debug build with attribution counters; logs `out/attr-warm-ge.log`,
`out/attr-warm2-ge.log`, `out/attr-cold-ge.log`)
**Spec:** `docs/superpowers/specs/2026-07-10-gespike-attribution-counters-design.md`

## Protocol

Three sessions, same shape (boot → Dam → shoot ≥4 distinct fresh guard/prop types):

1. **Warm-1**: driver `.vk.pcache` present (from item-1 testing: menu + brief play), but the
   session's combat content (blood/hit effects) never compiled on this device before.
2. **Warm-2**: immediate repeat — driver cache now contains session 1's combat pipelines
   (the item-1 debounce stored it 3× during session 1, 3.0→3.1MB).
3. **Cold-driver**: `.vk.pcache` deleted, guest `.xsh`/`.xpso` storage kept.

## Result: the first-shot hitch is blocking pipeline compilation, NOT file IO

| | Warm-1 | Warm-2 (repeat) | Cold-driver |
|---|---|---|---|
| Spikes >2ms in a first-use stage | 7 | 2 (both load/boot) | 10 |
| Worst combat first-shot frame | 222ms (**pcomp=92.2ms**, 5 pipelines) | **none** | 232ms (**pcomp=119.9ms**, 7 pipelines) |
| pcomp per pipeline | ~18ms (uncached) | ~0.34ms (cached, 70 pipelines in 23.6ms at level load) | ~17ms |
| strans worst | 17.5ms (24 shaders, the `_hits` event) | 36ms (38 shaders, load screen, gpu=0.2ms) | 5.5ms |
| gio (guest file IO) | **0 in all 310 spikes** | 0 | 0 |

Key evidence:

- The `_hits` probe storm (440 `NtCreateFile` probes at 10:39:26.5, warm-1) lands exactly on a
  133ms spike that translated **24 shaders (17.5ms) + compiled 34 pipelines (10.2ms)** + 83ms
  other CP work. The probes themselves cost microseconds — **the 2026-07-02 "file IO" theory
  was wrong**; the probe storm was the *marker* of the lazy `_hits` load, and the cost is the
  new shaders/pipelines/state the loaded models introduce. A `_hits` file prewarm (the original
  Phase 2B idea) would have fixed nothing.
- `gio=0` in every spike frame across all three sessions despite 748 NtCreateFile calls in
  warm-1 — total probe+read time fits inside normal frames. (Counter proven live on Linux:
  70ms/frame at boot.)
- Warm-2 is the smoking gun for item 1: with the driver cache covering the content, **combat
  first-shot hitches disappear entirely**. Adreno `vkCreateGraphicsPipelines` is ~18ms/pipeline
  uncached vs ~0.34ms cached (~50×).
- Cold-driver combat compiles include *new* shaders/pipelines (nshad 3-5 per event) even after
  two prior sessions — per-session state variance produces a tail of genuinely novel combos, so
  the `.xpso` boot precompile alone can't fully prevent combat compiles. But with a warm driver
  cache the tail is sub-millisecond.
- Shader translation is a minor cost on the Thor (≤17.5ms worst, usually ≤5ms) — the `.xsh`
  storage + boot precompile keeps it down. Level-load screens absorb the biggest strans bursts
  (36ms @ gpu=0.2ms — invisible to the player).

Steady-state overhead of the counters: none measurable (GEFPS avg 58.1 over a 36-min warm-1
session, in the normal band; desktop menu soak 57.4).

## Recommendation for Phase 2B

Target **blocking pipeline compilation**, in this order:

1. **Bundle starter guest storage in the APK** (`584108A9.xsh` + `.fbo.vk.xpso` grown from real
   playthroughs, copied to the cache dir on first boot if absent). First-install users then pay
   all known compiles during the boot loader screen, and the item-1 debounce persists the
   resulting driver blob immediately. Cheap (asset packaging + one copy-if-missing), big win
   for the first-session experience.
2. **Budget the CP-thread async-queue drain.** `CreateQueuedPipelinesOnProcessorThread` (called
   from `EndSubmission` when not startup-loading, "to reduce warm-up latency") drains queued
   *async* pipeline creations synchronously on the CP thread — on a 5-core handheld with slow
   Adreno compiles this converts "async" into a frame-end stall (it is one of the four
   instrumented blocking sites). Capping the drain (e.g. ≤2ms/frame, rest stays queued for the
   worker threads; placeholders already keep rendering correct) directly removes the 55-120ms
   combat frames whenever a compile does slip through the caches.
3. Not worth pursuing: `_hits` file prewarm (measured ~0 cost), texture-upload changes (≤1ms in
   every spike), shader-translation offload (minor, load-screen-absorbed).

A per-site split of `kPipelineCompileUs` (which of the four blocking sites dominates) would
sharpen item 2 if needed; the counters make that a 10-minute addition.

## Addendum (2026-07-11, shader-seed work)

The shader-seed feature exposed a pre-existing SDK bug that corrects one mechanism claim
above: **Android never actually loaded `.xsh`/`.xpso` across sessions** — `fopen("a+b")`'s
initial read position is EOF on bionic (start-of-file on glibc), so the storage header read
failed silently on every Android boot and the files were rebuilt from scratch each session.
The low `strans` in these captures came from within-session/load-screen translation, not
cross-session storage. Fixed in SDK `fix/android-storage-append-read` (explicit seeks);
combined with the bundled seed, a fresh install now precompiles at boot (verified: 173
pipelines, 2.64MB driver blob within 20s, combat `pcomp` clean over a 17-min acceptance
session; three menu-context compiles remain as seed-coverage gaps that future seed
refreshes absorb).
