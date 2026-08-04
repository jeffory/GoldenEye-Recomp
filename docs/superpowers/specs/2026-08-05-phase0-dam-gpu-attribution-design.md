# SPEC: Phase 0 — attribute the Dam fixed GPU cost

**Date:** 2026-08-05 · **Branch:** `develop` (`6f8ac48`) · **SDK checkout:** `../GoldenEye-Recomp-rexglue` (`479dc8e`)

**Status:** design approved, plan not yet written.

---

## 1. The question

Dam on the Ayn Thor (Adreno) is GPU-bound. `docs/HANDOFF-thor-gameplay-lockup.md:45` decomposes the
cost as **P≈6.5ms pixel-rate work + F≈13.5ms resolution-independent GPU work at 1×**.

That 13.5ms fixed cost is the single number gating every larger decision about this port's renderer:

- If it is **emulation-shaped** — EDRAM transfer passes, render-target ownership copies, resolves,
  per-draw command-processor overhead — then suppressing or replacing that work has real headroom,
  and the skate3recomp-style native renderer (`docs/RESEARCH-skate3-native-renderer.md`) has a case.
- If it is **raw geometry and binning** on the tiler, then a native renderer drawing the same
  geometry does not fix it. Its payoff narrows to the power/thermal axis alone, and the cheaper
  levers win instead.

Phase 0 answers that with measurement. It builds no renderer.

## 2. Decision rule (pre-registered)

Fixed up front, deliberately, so the result is not rationalized after the fact.

Two discriminators. They are not interchangeable — one yields a time delta, the other a volume
signal — so each gets its own rule.

**Primary, quantitative — path A/B sensitivity.** `render_target_path_vulkan`
(`src/graphics/vulkan/render_target_cache.cpp:40`, values `any|fbo|fsi`, init-only) selects between
host render targets and the fragment-shader-interlock path. These are two different implementations
of the *same* emulated EDRAM semantics, so a change in median GPU frame time between them is
attributable to the emulation layer by construction. **Verdict: emulation-shaped if the delta is
≥20% of the 13.5ms fixed cost (≥2.7ms).**

**Secondary, corroborating — EDRAM round-trip volume.** `vulkan_edram_roundtrip_stats` (`…:44`) logs
per-frame transfer passes, transfer draws, and host-depth store dispatches. Its own doc string names
the purpose: "measuring the tiler-killer overhead on arm64/tiled GPUs before/after a change
(HOM-149)." This is a *count*, not a time — it cannot on its own be converted to a share of the
13.5ms. It corroborates: sustained double-digit transfer passes per frame on Dam supports an
emulation-shaped reading; near-zero counts alongside an unchanged 13.5ms is strong evidence the cost
is geometry and binning instead.

**If `fsi` is unavailable on this Adreno** the primary discriminator is void, and counts alone cannot
carry the verdict. In that case Phase 0 reports *inconclusive* rather than guessing, and names the
follow-up: per-pass GPU timestamps bracketing the transfer/resolve passes. That is an SDK change and
is deliberately out of scope here (§7) — escalate it as its own decision rather than expanding this
phase mid-flight.

**Baseline validation.** Re-run with `resolution_scale` at 1 and 2 to confirm the 13.5ms + 6.5ms/1×
split holds on current `develop`. Note these cvars scale *up* from native (`resolution_scale`,
`draw_resolution_scale_x/y`, all default 1 = no scaling, `pipeline/texture/cache.cpp:67-77`), so the
lever is 1× → 2× — 4× pixel area, matching the `F+4P=39.5` measurement that produced the original
decomposition. If the split does not reproduce, that supersedes everything else and the attribution
restarts from the new decomposition.

## 3. What already exists

| Asset | Location | State |
|---|---|---|
| Deterministic Dam benchmark | `feat/input-replay` (`677d21b`, 9 commits, 927 LOC) | **Merges cleanly into `develop`** — only `src/ge_hooks.cpp` overlaps, `git merge-tree --write-tree` succeeds |
| Recording + menu macro | `bench/dam-walk.gerp`, `bench/dam.macro` | On that branch |
| Replay cvars | `ge_replay_play`, `ge_replay_macro`, `ge_bench_exit`, `ge_replay_probe` | `src/ge_replay.cpp:48-59` |
| GPU timestamps on Adreno | `ge_gpu_timestamps`, forced on for Android | `src/ge_app.h:97`; comment records it as validated on the Thor |
| Spike attribution | `GESPIKE … gpu={}us` | `src/ge_fps.cpp:305` |
| EDRAM round-trip counters | `EDRAM round-trips frame N: …` | SDK `render_target_cache.cpp:129` |
| Log analysis | `scripts/perf_report.py` | Parses GEFPS / GESHOWN / GESPIKE |

## 4. What is missing (the actual work)

### 4.1 GEBENCH does not report GPU time — blocking

`BenchOnPoll()` (`src/ge_replay.cpp:486-500`) accumulates exactly four counters: `kShaderTranslateUs`,
`kPipelineCompileUs`, `kTextureUploadUs`, `kGuestFileIoUs`. `kGpuFrameUs` is not among them, and the
periodic GEFPS line (`src/ge_fps.cpp:353`) has no `gpu=` field either.

The only steady-state-adjacent source of `kGpuFrameUs` is GESPIKE — which fires *only* on frames
exceeding 2× the rolling median. Those are by definition not the steady-state Dam frames under
attribution. Without this fix the benchmark produces a pacing report that cannot answer §1.

**Required:** sample `kGpuFrameUs` per frame in `BenchOnPoll()` under the existing
guest-refresh-advanced guard, retaining per-frame samples (not just a running sum) so GEBENCH can
report **median** GPU frame time — the median is what compares against the 13.5ms figure; a mean is
skewed by the hitches we are explicitly excluding. Add `kDrawCalls` on the same path: draw count per
frame is itself a discriminator between geometry-shaped and emulation-shaped cost.

### 4.2 Android cannot switch cvars without a rebuild

Android reads no config file and takes no CLI. Cvars are hardcoded `SetFlagByName` calls in
`GeApp::OnConfigurePaths` (`src/ge_app.h:63-106`), and `render_target_path_vulkan` is init-only. A
three-config A/B would therefore cost three rebuild-and-install cycles per repetition.

**Required:** read cvar overrides from a file under the app's external files directory during
`OnConfigurePaths`, applied after the existing hardcoded defaults so it can override them. `adb push`
then swaps configs between runs. This is small, self-contained, and removes the rebuild tax from
every future on-device experiment, not just this one.

### 4.3 perf_report.py parses neither GEBENCH nor the EDRAM lines

**Required:** add both parsers. EDRAM round-trip lines are per-frame and only emitted when nonzero;
aggregating them in the existing script is cheaper and lower-risk than adding SDK-side accumulators,
and keeps Phase 0 free of SDK changes.

## 5. Protocol

- **Workload:** `bench/dam-walk.gerp` via `ge_replay_play`, entered through `ge_replay_macro`
  (`bench/dam.macro`), `ge_bench_exit=true` for unattended runs.
- **Configs:** current default, `render_target_path_vulkan=fbo`, `render_target_path_vulkan=fsi`;
  plus the default config at `resolution_scale=2` for the fixed/fill split (§2). The scale sweep does
  not need repeating across all three paths — it validates the baseline decomposition, it is not part
  of the path A/B.
- **Repetitions:** 3 per config.
- **Run order:** interleaved or randomized across configs — never all of one config then the next.
  Paint time is documented degrading 4.8→36ms over minutes (suspected thermal), so blocked run order
  would confound config with device temperature. Log ambient/session start time per run.
- **Capture:** `ge.log` per run — GEBENCH, GEFPS, GESHOWN, GESPIKE, EDRAM round-trip lines, with
  `vulkan_edram_roundtrip_stats=true` and `ge_gpu_timestamps=true`.
- **Analysis:** `scripts/perf_report.py`, extended per §4.3.
- **Verification:** release-build confirmation before trusting any on-device result — debug and
  release APKs diverge on SELinux-gated behavior (see `android-release-vs-debug-selinux-ptrace`
  memory; that difference silently broke a shipped feature once already).

## 6. Risks

| Risk | Mitigation |
|---|---|
| `bench/dam-walk.gerp` was recorded on desktop; the macro's flag-waiting may need retiming on Android | Most likely cost sink — budget a day. `ge_replay_probe` logs poll cadence for diagnosis |
| Thermal drift confounds config comparison | Interleaved run order (§5); report per-run start temperature if obtainable |
| `fsi` path may not be selectable on this Adreno | Path selection auto-falls-back and logs the reason (`render_target_cache.cpp:355-395`) — capture that log line. If `fsi` is unavailable the primary discriminator is void and Phase 0 reports *inconclusive* with a named follow-up (§2), rather than resting a verdict on counts alone |
| Replay determinism drift over a multi-minute run | GEBENCH frame count and duration are the guard: runs whose frame counts differ materially are discarded, not averaged |

## 7. Out of scope

Deliberately excluded so this stays one plan:

- **The cheaper perf levers** — frame-cap/pacing for GPU-bound levels, budgeting
  `CreateQueuedPipelinesOnProcessorThread` on the CP thread, the open quick-restart skip-bit latch.
  Independent of this measurement, each deserves its own plan. Queued, not abandoned.
- **Selective suppression** of specific expensive guest passes. skate3's own cvar docs report that
  suppressing their postfx chain alone was ≈2×, so this is the highest value-per-effort option *if*
  Phase 0 identifies an analogous dominant pass — but it is conditional on this result.
- **The native renderer itself.** ~37k LOC equivalent with zero render-structure RE done. Gated on
  this verdict.
- **Any SDK change.** Phase 0 is game-side plus log analysis only.

## 8. Success criteria

Phase 0 is done when a written verdict states, with per-run numbers attached:

1. Whether the 13.5ms + 6.5ms/1× decomposition reproduces on current `develop`.
2. Median GPU frame time on Dam per config, with variance across the three repetitions.
3. EDRAM transfer passes / transfer draws / host-depth dispatches per frame on Dam.
4. One of three verdicts per the §2 rules — **emulation-shaped**, **geometry-bound**, or
   **inconclusive** (`fsi` unavailable) — and therefore go / no-go / escalate for §7's conditional
   work.

## 9. Sources

- `docs/RESEARCH-skate3-native-renderer.md` — feasibility study this phase gates (2026-07-26);
  appendices in `docs/research/`.
- `docs/HANDOFF-thor-gameplay-lockup.md:45,49` — origin of the 13.5ms / 6.5ms decomposition.
- skate3recomp `v2.0.2` (2026-07-24) and `mchughalex/rexglue-skate3` (`7eb0faf`) — both quiet since
  2026-07-24 as of this writing. Their approach **suppresses** the emulated path behind
  `native_render_suppress_mode` while keeping the CP, ring, swap counter, and vblank alive; it does
  not delete it. That distinction is load-bearing here: `ge_dbg_now` (`src/ge_hooks.cpp:685`) derives
  frame completion from CP internals and writes the guest's GPU bookkeeping itself, so a
  `graphics == nullptr` runtime (SDK `src/system/runtime.cpp:163`) would latch its 80ms skip-bit
  fallback permanently — the same freeze class already fought on Linux and arm64.
