# GESPIKE Attribution Counters (Perf Deep-Dive Item 2, Phase 2A) — Design

**Date:** 2026-07-10
**Repos touched:** SDK (`../GoldenEye-Recomp-rexglue`) + game (`src/ge_fps.cpp`, `scripts/perf_report.py`)
**Status:** Approved
**Follow-up:** Phase 2B (`_hits` prewarm) is designed only after this phase's capture data exists.

## Problem

The first-shot-at-an-enemy hitch (GESPIKE `dt=173.7ms cpexec=87ms gpu=3ms` coinciding with
lazy `prop\chr*_hits` / `head*_hits` loads) cannot be attributed further: shader translation,
pipeline compilation, and texture upload all fold into `cpexec`, and guest-thread file IO is
invisible except for warning-log side effects. We must not design the prewarm blind —
especially since the just-shipped pipeline-cache persistence may already have removed part of
the hitch.

## Design

### New counters (SDK, `include/rex/perf/counter.h` + `src/core/perf/counter.cpp`)

Six new `CounterId` entries appended after the existing frame-stage block, with CSV column
names, all behind the existing `REXGLUE_ENABLE_PERF_COUNTERS` gate, using the existing
accumulate-per-frame / snapshot-on-`Profiler::Flip()` model:

| CounterId | CSV name | Measures |
|---|---|---|
| `kShaderTranslateUs` | `shader_translate_us` | Xenos→SPIR-V translation wall time on the CP thread (`EnsureShadersTranslated` → `TranslateAnalyzedShader` call sites, `src/graphics/vulkan/pipeline_cache.cpp:1180-1195`) |
| `kPipelineCompileUs` | `pipeline_compile_us` | **CP-thread-blocking** `EnsurePipelineCreated` wall time only: the sync-fallback call sites in `ConfigurePipeline` and `CreateQueuedPipelinesOnProcessorThread` (called from `EndSubmission`). Worker-thread compiles are deliberately NOT counted — they don't stall the frame |
| `kTextureUploadUs` | `texture_upload_us` | CP-thread texture-cache load path (`TextureCache::RequestTextures` → `CommitPreparedTextureLoad` / `LoadTextureData`, `src/graphics/pipeline/texture/cache.cpp:578,583`) |
| `kGuestFileIoUs` | `guest_file_io_us` | Kernel IO dispatch wall time accumulated across guest threads: `NtCreateFile_entry`, `NtOpenFile_entry` (delegates to NtCreateFile — must not double-count), `NtReadFile_entry` in `src/kernel/xboxkrnl/xboxkrnl_io.cpp`. Thread-time accumulator like the existing `kGuestGpuWaitUs` — may exceed wall time |
| `kShadersTranslated` | `shaders_translated` | Count of shaders translated that frame |
| `kPipelinesCompiled` | `pipelines_compiled` | Count of CP-thread-blocking pipeline compiles that frame |

New `PROFILE_*` macros follow the existing pattern (`counter.h:144-208`); timing = two
`steady_clock` reads per event, only on first-use/IO events. `NtOpenFile_entry` forwards to
`NtCreateFile_entry` internally: instrument `NtCreateFile_entry` and `NtReadFile_entry`
bodies only — the NtOpenFile forward then inherits its timing through NtCreateFile with no
double count.

### Game surfacing (`src/ge_fps.cpp`)

GESPIKE gains six fields, appended after `wwf=`:

```
GESPIKE ... wwf={} strans={}us pcomp={}us texup={}us gio={}us nshad={} npipe={}
```

The GEWATCHDOG TOTAL-FREEZE stage dump (`src/ge_hooks.cpp`) prints the same snapshot
counters; add the same six there. CSV columns appear automatically via the CounterId name
table.

### Report tooling (`scripts/perf_report.py`)

- Extend the GESPIKE regex for the six new fields (backward-compatible: fields optional so
  old logs still parse).
- Add `strans`, `pcomp`, `texup`, `gio` to the `STAGES` map so spike clustering can name
  them. Like `wrm`, `strans`/`pcomp`/`texup` nest inside `cpexec`: extend the existing
  net-CP derivation to `cp_execute_net_us = max(0, cpexec − wrm − strans − pcomp − texup)`.
  `gio` is guest-thread time, not part of `cpexec`.

### Capture protocol (the phase's real deliverable)

On the Thor (debug build already installed), `ge_spike_log` is already forced on:
1. **Warm run** (pipeline cache present from item 1): fresh boot → Dam → shoot ≥4 distinct
   fresh enemy/prop types → pull ge.log.
2. **Cold run**: delete `*.vk.pcache` on device → same session shape → pull ge.log.
3. Optionally enable `perf_log_csv` via `SetFlagByName` in `GeApp::OnConfigurePaths` for one
   run to get per-frame CSV.
4. Run `perf_report.py` on both; write a short findings note
   (`docs/superpowers/<capture-date>-first-shot-hitch-attribution.md`, dated when the
   capture runs) stating, per first-shot
   spike: how many µs were `gio` vs `strans` vs `pcomp` vs `texup` vs unattributed `cpexec`,
   and how the warm/cold pipeline cache changes it. That note is Phase 2B's input.

## Success criteria

- Spike lines on device show the new fields; a first-shot spike's µs sum meaningfully
  attributes the previously opaque `cpexec` (unattributed remainder clearly smaller than the
  attributed parts, or itself the finding).
- Zero measurable overhead in steady-state play (counters only tick on first-use events and
  file IO; GEFPS avg unchanged within noise on a 60s soak).
- Old logs still parse in `perf_report.py`.

## Risks

Low. Additive counters behind an existing compile gate; no behavior changes. Care points:
don't double-count NtOpenFile→NtCreateFile forwarding; count only CP-thread-blocking
pipeline compiles; keep GESPIKE line under log-line length limits (it grows ~60 chars).
