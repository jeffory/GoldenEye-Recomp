# GESPIKE Attribution Counters Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make first-use costs (shader translation, blocking pipeline compile, texture upload, guest file IO) visible per-frame in GESPIKE/CSV/perf_report so the first-shot hitch can be attributed with data instead of guesses.

**Architecture:** Six new `rex::perf` counters in the SDK (accumulate-per-frame, snapshotted by `Profiler::Flip()` like the existing frame-stage counters), fed by a new RAII scoped-timer macro at four instrumentation sites; the game appends the six values to the GESPIKE line and the GEWATCHDOG stage dump; `perf_report.py` parses the new fields, nets them out of `cpexec`, and can cluster spikes by them. Spec: `docs/superpowers/specs/2026-07-10-gespike-attribution-counters-design.md`.

**Tech Stack:** C++17 (SDK + game), Python 3 stdlib (report script), CMake preset `linux-amd64-relwithdebinfo`, Android gradle.

## Global Constraints

- **PAIRED BRANCHES:** SDK work on branch `feat/gespike-attribution` off `main` in `/home/keith/Projects/GoldenEye-Recomp-rexglue`; game work on branch `feat/gespike-attribution` off `develop` in `/home/keith/Projects/GoldenEye-Recomp`. They must merge together (game GESPIKE code references the new `CounterId`s).
- Do NOT use git worktrees (the game build hard-references the sibling SDK path).
- All new counters/macros go behind the existing `REXGLUE_ENABLE_PERF_COUNTERS` gate; disabled builds must compile to no-ops.
- Counter enum order and the `kCounterNames` / `kIsGauge` tables must stay in sync (existing `static_assert`s enforce length).
- `kPipelineCompileUs` counts **CP-thread-blocking** compiles only: the three `EnsurePipelineCreated` call sites inside `ConfigurePipeline` and the one inside `CreateQueuedPipelinesOnProcessorThread`. The `CreationThread` worker call site and the shader-storage bulk-load call sites are explicitly NOT counted.
- Kernel IO: instrument the bodies of `NtCreateFile_entry` and `NtReadFile_entry` only; `NtOpenFile_entry` forwards to `NtCreateFile_entry` and inherits timing (no double count).
- GESPIKE line grows exactly: ` strans={}us pcomp={}us texup={}us gio={}us nshad={} npipe={}` appended after `wwf={}`.
- `perf_report.py` must still parse old-format logs (verify against `out/session-2026-07-02-c/ge.log`, expected 92 GESPIKE lines).
- Build: `cd /home/keith/Projects/GoldenEye-Recomp && cmake --build --preset linux-amd64-relwithdebinfo --target ge`. A build whose tail says only "ninja: no work to do" does NOT prove your edit compiled — require a `Building CXX object ...` line for each edited file.
- No unit-test framework exists for SDK/game C++; those tasks verify by build + the runtime smoke in Task 5. `perf_report.py` IS testable with real commands — Task 4 does test-first.

---

### Task 1: SDK counter registry (enum, names, gauge table, macros)

**Files:**
- Modify: `/home/keith/Projects/GoldenEye-Recomp-rexglue/include/rex/perf/counter.h`
- Modify: `/home/keith/Projects/GoldenEye-Recomp-rexglue/src/core/perf/counter.cpp`

**Interfaces:**
- Consumes: existing `CounterId`, `IncrementCounter`, `PERF_counter_inc/add` patterns.
- Produces (used by Tasks 2–3): `CounterId::kShaderTranslateUs`, `kPipelineCompileUs`, `kTextureUploadUs`, `kGuestFileIoUs`, `kShadersTranslated`, `kPipelinesCompiled`; macros `PROFILE_SCOPED_US(id)` (RAII, adds elapsed µs to `id` at scope exit), `PROFILE_SHADER_TRANSLATED()`, `PROFILE_PIPELINE_COMPILED()`. CSV columns `shader_translate_us`, `pipeline_compile_us`, `texture_upload_us`, `guest_file_io_us`, `shaders_translated`, `pipelines_compiled` appear automatically (CSV writer iterates all counters).

- [ ] **Step 1: Create both feature branches**

```bash
cd /home/keith/Projects/GoldenEye-Recomp-rexglue && git checkout main && git checkout -b feat/gespike-attribution
cd /home/keith/Projects/GoldenEye-Recomp && git checkout develop && git checkout -b feat/gespike-attribution
```

- [ ] **Step 2: Extend the CounterId enum**

In `include/rex/perf/counter.h`, after the `kGpuFrameUs,` line and before `kCount`, insert:

```cpp
  // First-use attribution (us per frame) -- split cpexec spikes into shader
  // translation / blocking pipeline compilation / texture upload, plus guest
  // file IO (accumulated across guest threads like kGuestGpuWaitUs, so it is
  // thread-time and may exceed wall time).
  kShaderTranslateUs,   // CP worker: Xenos->SPIR-V first-encounter translation
  kPipelineCompileUs,   // CP worker: frame-blocking vkCreateGraphicsPipelines
  kTextureUploadUs,     // CP worker: texture-cache load commits
  kGuestFileIoUs,       // guest threads: NtCreateFile/NtReadFile dispatch
  kShadersTranslated,   // count of shaders translated this frame
  kPipelinesCompiled,   // count of frame-blocking pipeline compiles this frame
```

- [ ] **Step 3: Add `<chrono>` and the scoped-timer class to counter.h**

Add `#include <chrono>` after `#include <cstdint>` (line 13). Then, inside `namespace rex::perf` directly after the `Profiler` class (before the closing `}  // namespace rex::perf`), add:

```cpp
// RAII helper: adds elapsed microseconds to a counter at scope exit. Two
// steady_clock reads per instance -- use only around first-use/IO events,
// not per-draw hot paths.
class ScopedCounterTimer {
 public:
  explicit ScopedCounterTimer(CounterId id)
      : id_(id), start_(std::chrono::steady_clock::now()) {}
  ~ScopedCounterTimer() {
    IncrementCounter(id_, std::chrono::duration_cast<std::chrono::microseconds>(
                              std::chrono::steady_clock::now() - start_)
                              .count());
  }
  ScopedCounterTimer(const ScopedCounterTimer&) = delete;
  ScopedCounterTimer& operator=(const ScopedCounterTimer&) = delete;

 private:
  CounterId id_;
  std::chrono::steady_clock::time_point start_;
};
```

- [ ] **Step 4: Add the macros**

In the `#ifdef REXGLUE_ENABLE_PERF_COUNTERS` block, after `#define PROFILE_GPU_FRAME_US(v) ...`, add:

```cpp
#define PERF_SCOPED_CAT2(a, b) a##b
#define PERF_SCOPED_CAT(a, b) PERF_SCOPED_CAT2(a, b)
#define PROFILE_SCOPED_US(id)                                          \
  rex::perf::ScopedCounterTimer PERF_SCOPED_CAT(_perf_scoped_, __LINE__)( \
      rex::perf::CounterId::id)
#define PROFILE_SHADER_TRANSLATED() PERF_counter_inc(kShadersTranslated)
#define PROFILE_PIPELINE_COMPILED() PERF_counter_inc(kPipelinesCompiled)
```

In the `#else` block, after `#define PROFILE_GPU_FRAME_US(v)`, add:

```cpp
#define PROFILE_SCOPED_US(id)
#define PROFILE_SHADER_TRANSLATED()
#define PROFILE_PIPELINE_COMPILED()
```

- [ ] **Step 5: Extend the names and gauge tables in counter.cpp**

In `src/core/perf/counter.cpp`, `kCounterNames`: after `"gpu_frame_us",` add:

```cpp
    "shader_translate_us",
    "pipeline_compile_us",
    "texture_upload_us",
    "guest_file_io_us",
    "shaders_translated",
    "pipelines_compiled",
```

In `kIsGauge`: after `false,  // kGpuFrameUs ...` add:

```cpp
    false,  // kShaderTranslateUs (accumulated per frame)
    false,  // kPipelineCompileUs (accumulated per frame)
    false,  // kTextureUploadUs   (accumulated per frame)
    false,  // kGuestFileIoUs     (accumulated per frame, thread-time)
    false,  // kShadersTranslated (count per frame)
    false,  // kPipelinesCompiled (count per frame)
```

(The existing `static_assert`s will fail the build if either table is out of sync.)

- [ ] **Step 6: Build**

```bash
cd /home/keith/Projects/GoldenEye-Recomp && cmake --build --preset linux-amd64-relwithdebinfo --target ge
```
Expected: `Building CXX object .../counter.cpp.o` appears, clean link.

- [ ] **Step 7: Commit**

```bash
cd /home/keith/Projects/GoldenEye-Recomp-rexglue
git add include/rex/perf/counter.h src/core/perf/counter.cpp
git commit -m "feat(perf): first-use attribution counters + scoped-timer macro"
```

---

### Task 2: SDK instrumentation sites

**Files:**
- Modify: `/home/keith/Projects/GoldenEye-Recomp-rexglue/src/graphics/vulkan/pipeline_cache.cpp` (EnsureShadersTranslated ~line 1212; ConfigurePipeline call sites ~1347, 1384, 1410; CreateQueuedPipelinesOnProcessorThread ~3829)
- Modify: `/home/keith/Projects/GoldenEye-Recomp-rexglue/include/rex/graphics/vulkan/pipeline_cache.h` (~line 368, next to `EnsurePipelineCreated`)
- Modify: `/home/keith/Projects/GoldenEye-Recomp-rexglue/src/graphics/pipeline/texture/cache.cpp` (~lines 576-586)
- Modify: `/home/keith/Projects/GoldenEye-Recomp-rexglue/src/kernel/xboxkrnl/xboxkrnl_io.cpp` (NtCreateFile_entry ~103, NtReadFile_entry ~187)

**Interfaces:**
- Consumes: Task 1's `PROFILE_SCOPED_US`, `PROFILE_SHADER_TRANSLATED`, `PROFILE_PIPELINE_COMPILED` (from `<rex/perf/counter.h>`).
- Produces: new private method `bool VulkanPipelineCache::EnsurePipelineCreatedBlocking(const PipelineCreationArguments& creation_arguments, VkShaderModule fragment_shader_override = VK_NULL_HANDLE)` — same contract as `EnsurePipelineCreated`, plus timing/count.

- [ ] **Step 1: Includes**

Add `#include <rex/perf/counter.h>` to the include blocks of `src/graphics/vulkan/pipeline_cache.cpp`, `src/graphics/pipeline/texture/cache.cpp`, and `src/kernel/xboxkrnl/xboxkrnl_io.cpp` (alphabetical position within their `<rex/...>` groups).

- [ ] **Step 2: Shader translation timing**

In `EnsureShadersTranslated` (`pipeline_cache.cpp:1220-1243`), change both translate-if blocks:

```cpp
  if (!vertex_shader->is_translated()) {
    PROFILE_SCOPED_US(kShaderTranslateUs);
    PROFILE_SHADER_TRANSLATED();
    vertex_shader->shader().AnalyzeUcode(ucode_disasm_buffer_);
    if (!TranslateAnalyzedShader(*shader_translator_, *vertex_shader)) {
      REXGPU_ERROR("Failed to translate the vertex shader!");
      return false;
    }
  }
```

and

```cpp
    if (!pixel_shader->is_translated()) {
      PROFILE_SCOPED_US(kShaderTranslateUs);
      PROFILE_SHADER_TRANSLATED();
      pixel_shader->shader().AnalyzeUcode(ucode_disasm_buffer_);
      if (!TranslateAnalyzedShader(*shader_translator_, *pixel_shader)) {
        REXGPU_ERROR("Failed to translate the pixel shader!");
        return false;
      }
    }
```

(Only the two `PROFILE_` lines are new; AnalyzeUcode is deliberately inside the timed scope — it's part of first-encounter cost.)

- [ ] **Step 3: Blocking-compile wrapper**

In `include/rex/graphics/vulkan/pipeline_cache.h`, directly after the `EnsurePipelineCreated` declaration (line ~368-369), add:

```cpp
  // EnsurePipelineCreated plus kPipelineCompileUs/kPipelinesCompiled
  // accounting. Use at CP-thread call sites only -- worker-thread compiles
  // do not block the frame and must not be counted.
  bool EnsurePipelineCreatedBlocking(const PipelineCreationArguments& creation_arguments,
                                     VkShaderModule fragment_shader_override = VK_NULL_HANDLE);
```

In `src/graphics/vulkan/pipeline_cache.cpp`, directly above `bool VulkanPipelineCache::EnsurePipelineCreated(...)` (~line 3246), add:

```cpp
bool VulkanPipelineCache::EnsurePipelineCreatedBlocking(
    const PipelineCreationArguments& creation_arguments,
    VkShaderModule fragment_shader_override) {
  PROFILE_SCOPED_US(kPipelineCompileUs);
  PROFILE_PIPELINE_COMPILED();
  return EnsurePipelineCreated(creation_arguments, fragment_shader_override);
}
```

- [ ] **Step 4: Swap the four CP-thread call sites to the wrapper**

1. `ConfigurePipeline` sync fallback (~1347): `!EnsurePipelineCreated(creation_arguments)` → `!EnsurePipelineCreatedBlocking(creation_arguments)`
2. `ConfigurePipeline` placeholder create (~1384): `EnsurePipelineCreated(creation_arguments_placeholder, placeholder_pixel_shader_)` → `EnsurePipelineCreatedBlocking(creation_arguments_placeholder, placeholder_pixel_shader_)`
3. `ConfigurePipeline` no-async fallback (~1410): `!EnsurePipelineCreated(creation_arguments_real)` → `!EnsurePipelineCreatedBlocking(creation_arguments_real)`
4. `CreateQueuedPipelinesOnProcessorThread` (~3829): `EnsurePipelineCreated(creation_arguments)` → `EnsurePipelineCreatedBlocking(creation_arguments)`

Do NOT touch the call sites in `CreationThread` (~3790) or the shader-storage bulk load (~840).

- [ ] **Step 5: Texture upload timing**

In `src/graphics/pipeline/texture/cache.cpp` `RequestTextures` (~576-586), add a per-iteration scoped timer to both commit loops (clock reads only happen when a load actually occurs):

```cpp
  if (batched_shared_memory_request_succeeded) {
    for (const PendingTextureLoad& pending_load : pending_texture_loads) {
      PROFILE_SCOPED_US(kTextureUploadUs);
      CommitPreparedTextureLoad(pending_load);
    }
  } else {
    for (const PendingTextureLoad& pending_load : pending_texture_loads) {
      if (pending_load.texture != nullptr) {
        PROFILE_SCOPED_US(kTextureUploadUs);
        LoadTextureData(*pending_load.texture);
      }
    }
  }
```

- [ ] **Step 6: Guest file IO timing**

In `src/kernel/xboxkrnl/xboxkrnl_io.cpp`, add as the FIRST statement of `NtCreateFile_entry`'s body (line ~108, before the commented-out allocation_size block):

```cpp
  PROFILE_SCOPED_US(kGuestFileIoUs);
```

and the same as the FIRST statement of `NtReadFile_entry`'s body (~line 190, before the `byte_offset` line). Do NOT touch `NtOpenFile_entry` (it forwards to NtCreateFile_entry and inherits the timing).

- [ ] **Step 7: Build**

```bash
cd /home/keith/Projects/GoldenEye-Recomp && cmake --build --preset linux-amd64-relwithdebinfo --target ge
```
Expected: `Building CXX object` lines for `pipeline_cache.cpp`, `cache.cpp`, `xboxkrnl_io.cpp`; clean link.

- [ ] **Step 8: Commit**

```bash
cd /home/keith/Projects/GoldenEye-Recomp-rexglue
git add include/rex/perf/counter.h include/rex/graphics/vulkan/pipeline_cache.h \
        src/graphics/vulkan/pipeline_cache.cpp src/graphics/pipeline/texture/cache.cpp \
        src/kernel/xboxkrnl/xboxkrnl_io.cpp
git commit -m "feat(perf): instrument shader translate, blocking pipeline compile, texture upload, guest file IO"
```

---

### Task 3: Game surfacing (GESPIKE + GEWATCHDOG fields)

**Files:**
- Modify: `/home/keith/Projects/GoldenEye-Recomp/src/ge_fps.cpp:304-317` (GESPIKE emit)
- Modify: `/home/keith/Projects/GoldenEye-Recomp/src/ge_hooks.cpp:353-362` (GEWATCHDOG TOTAL-FREEZE stage dump)

**Interfaces:**
- Consumes: Task 1's six `CounterId`s via the already-included `<rex/perf/counter.h>` and `GetSnapshotCounter`.
- Produces: the exact GESPIKE grammar Task 4's regex parses: `... starved={} wwf={} strans={}us pcomp={}us texup={}us gio={}us nshad={} npipe={}`.

- [ ] **Step 1: Extend the GESPIKE line in ge_fps.cpp**

Replace the `REXKRNL_INFO(` call at lines 304-317 with:

```cpp
        REXKRNL_INFO(
            "GESPIKE dt={:.1f}ms med={:.1f}ms cpexec={}us cpidle={}us "
            "wrm={}us present={}us gwait={}us gpu={}us draws={} stalls={} "
            "starved={} wwf={} strans={}us pcomp={}us texup={}us gio={}us "
            "nshad={} npipe={}",
            per / 1000.0, med / 1000.0,
            GetSnapshotCounter(CounterId::kCpExecuteUs),
            GetSnapshotCounter(CounterId::kCpIdleUs),
            GetSnapshotCounter(CounterId::kCpWaitRegMemUs),
            GetSnapshotCounter(CounterId::kPresentBlockUs),
            GetSnapshotCounter(CounterId::kGuestGpuWaitUs),
            GetSnapshotCounter(CounterId::kGpuFrameUs),
            GetSnapshotCounter(CounterId::kDrawCalls),
            GetSnapshotCounter(CounterId::kCommandBufferStalls),
            GetCpStarvedEpisodes(), wwf_frame,
            GetSnapshotCounter(CounterId::kShaderTranslateUs),
            GetSnapshotCounter(CounterId::kPipelineCompileUs),
            GetSnapshotCounter(CounterId::kTextureUploadUs),
            GetSnapshotCounter(CounterId::kGuestFileIoUs),
            GetSnapshotCounter(CounterId::kShadersTranslated),
            GetSnapshotCounter(CounterId::kPipelinesCompiled));
```

- [ ] **Step 2: Extend the GEWATCHDOG stage dump in ge_hooks.cpp**

Replace the `REXKRNL_INFO(` call at lines ~353-362 (the "TOTAL-FREEZE: last-frame stages" one) with:

```cpp
          REXKRNL_INFO(
              "GEWATCHDOG TOTAL-FREEZE: last-frame stages cpexec={}us cpidle={}us wrm={}us "
              "present={}us gwait={}us gpu={}us strans={}us pcomp={}us texup={}us gio={}us "
              "| if a guest thread is burning CPU, attach with: "
              "run-as com.sunjaycy.goldeneye simpleperf record -g -p <pid>",
              rex::perf::GetSnapshotCounter(rex::perf::CounterId::kCpExecuteUs),
              rex::perf::GetSnapshotCounter(rex::perf::CounterId::kCpIdleUs),
              rex::perf::GetSnapshotCounter(rex::perf::CounterId::kCpWaitRegMemUs),
              rex::perf::GetSnapshotCounter(rex::perf::CounterId::kPresentBlockUs),
              rex::perf::GetSnapshotCounter(rex::perf::CounterId::kGuestGpuWaitUs),
              rex::perf::GetSnapshotCounter(rex::perf::CounterId::kGpuFrameUs),
              rex::perf::GetSnapshotCounter(rex::perf::CounterId::kShaderTranslateUs),
              rex::perf::GetSnapshotCounter(rex::perf::CounterId::kPipelineCompileUs),
              rex::perf::GetSnapshotCounter(rex::perf::CounterId::kTextureUploadUs),
              rex::perf::GetSnapshotCounter(rex::perf::CounterId::kGuestFileIoUs));
```

- [ ] **Step 3: Build**

```bash
cd /home/keith/Projects/GoldenEye-Recomp && cmake --build --preset linux-amd64-relwithdebinfo --target ge
```
Expected: `Building CXX object` for `ge_fps.cpp` and `ge_hooks.cpp`, clean link.

- [ ] **Step 4: Commit (game repo, game branch)**

```bash
cd /home/keith/Projects/GoldenEye-Recomp
git add src/ge_fps.cpp src/ge_hooks.cpp
git commit -m "feat(perf): surface first-use attribution counters in GESPIKE + watchdog dump"
```

---

### Task 4: perf_report.py — parse, net-out, cluster (test-first)

**Files:**
- Modify: `/home/keith/Projects/GoldenEye-Recomp/scripts/perf_report.py:28-58` (STAGES, derive_stages, GESPIKE_RE) and `:84-87` (None-safe group conversion)

**Interfaces:**
- Consumes: Task 3's exact GESPIKE grammar.
- Produces: spike dicts with keys `shader_translate_us`, `pipeline_compile_us`, `texture_upload_us`, `guest_file_io_us`, `nshad`, `npipe` (0.0 when absent); `cp_execute_net_us` netted of the three new nested stages.

- [ ] **Step 1: Write the failing test (synthetic new-format + old-format regression)**

```bash
cd /home/keith/Projects/GoldenEye-Recomp
cat > /tmp/claude-1000/-home-keith-Projects-GoldenEye-Recomp/e13e463a-cbdf-4b94-a278-02419ca85208/scratchpad/gespike-new.log <<'EOF'
[2026-07-11 12:00:00.000] [info] [krnl] [t1] GESPIKE dt=120.0ms med=16.7ms cpexec=90000us cpidle=100us wrm=10000us present=5000us gwait=2000us gpu=3000us draws=500 stalls=1 starved=0 wwf=2 strans=40000us pcomp=15000us texup=8000us gio=30000us nshad=3 npipe=5
EOF
python3 - <<'EOF'
import sys; sys.path.insert(0, "scripts")
import perf_report as pr
m = pr.GESPIKE_RE.search(open("/tmp/claude-1000/-home-keith-Projects-GoldenEye-Recomp/e13e463a-cbdf-4b94-a278-02419ca85208/scratchpad/gespike-new.log").read())
assert m, "new-format line did not match"
d = pr.derive_stages({k: (float(v) if v is not None else 0.0) for k, v in m.groupdict().items()})
assert d.get("shader_translate_us") == 40000.0, d
assert d.get("guest_file_io_us") == 30000.0, d
# net cpexec = 90000 - 10000(wrm) - 40000(strans) - 15000(pcomp) - 8000(texup) = 17000
assert d["cp_execute_net_us"] == 17000.0, d
assert "shader_translate_us" in pr.STAGES and "guest_file_io_us" in pr.STAGES
print("NEW-FORMAT OK")
EOF
```

Run it. Expected: **FAIL** — `assert d.get("shader_translate_us") == 40000.0` (the field isn't in the regex yet, groupdict lacks the key → `None`/missing).

- [ ] **Step 2: Implement the parser changes**

In `scripts/perf_report.py`:

a) `STAGES` (after the `"gpu_frame_us"` entry):

```python
    "shader_translate_us": "Shader translation (Xenos->SPIR-V, first encounter)",
    "pipeline_compile_us": "Pipeline compile (frame-blocking vkCreateGraphicsPipelines)",
    "texture_upload_us": "Texture upload (first-seen texture loads)",
    "guest_file_io_us": "Guest file IO (NtCreateFile/NtReadFile, thread-time)",
```

b) `derive_stages` becomes:

```python
def derive_stages(fr):
    """Add derived stage fields to a frame/spike dict (mutates + returns it)."""
    # max(0, ...) keeps the operand type: int for CSV rows, float for log lines.
    # strans/pcomp/texup nest inside cpexec (they run on the CP thread inside
    # ExecutePrimaryBuffer), same as the WAIT_REG_MEM fence; gio does not (guest
    # threads).
    fr["cp_execute_net_us"] = max(
        0, fr.get("cp_execute_us", 0) - fr.get("cp_wait_reg_mem_us", 0)
           - fr.get("shader_translate_us", 0) - fr.get("pipeline_compile_us", 0)
           - fr.get("texture_upload_us", 0))
    return fr
```

c) `GESPIKE_RE` — append an optional tail after `starved=(?P<starved>\d+)`:

```python
GESPIKE_RE = re.compile(
    r"GESPIKE dt=(?P<dt>[\d.]+)ms med=(?P<med>[\d.]+)ms "
    r"cpexec=(?P<cp_execute_us>\d+)us cpidle=(?P<cp_idle_us>\d+)us "
    r"wrm=(?P<cp_wait_reg_mem_us>\d+)us present=(?P<present_block_us>\d+)us "
    r"gwait=(?P<guest_gpu_wait_us>\d+)us gpu=(?P<gpu_frame_us>\d+)us "
    r"draws=(?P<draws>\d+) stalls=(?P<stalls>\d+) starved=(?P<starved>\d+)"
    r"(?: wwf=(?P<wwf>\d+) strans=(?P<shader_translate_us>\d+)us "
    r"pcomp=(?P<pipeline_compile_us>\d+)us texup=(?P<texture_upload_us>\d+)us "
    r"gio=(?P<guest_file_io_us>\d+)us nshad=(?P<nshad>\d+) npipe=(?P<npipe>\d+))?")
```

d) None-safe conversion in `analyze_log` (line ~87) — unmatched optional groups return `None`, and `float(None)` crashes:

```python
            m = GESPIKE_RE.search(line)
            if m:
                gespikes.append(derive_stages(
                    {k: (float(v) if v is not None else 0.0)
                     for k, v in m.groupdict().items()}))
```

- [ ] **Step 3: Run the test — expect PASS**

Re-run Step 1's python block. Expected: `NEW-FORMAT OK`.

- [ ] **Step 4: Old-format regression**

```bash
cd /home/keith/Projects/GoldenEye-Recomp
python3 scripts/perf_report.py out/session-2026-07-02-c/ge.log
```
Expected: runs without traceback, prints `Spikes: 92 GESPIKE lines` and cluster lines (old logs lack the new fields; they parse with the fields absent → 0.0).

- [ ] **Step 5: Commit**

```bash
git add scripts/perf_report.py
git commit -m "feat(perf): parse + cluster first-use attribution fields in perf_report"
```

---

### Task 5: Linux runtime smoke (end-to-end counters live)

**Files:** none (verification only).

**Interfaces:**
- Consumes: the built binary at `out/build/linux-amd64-relwithdebinfo/GoldenEye`; run recipe `LD_LIBRARY_PATH=../GoldenEye-Recomp-rexglue/out/linux-amd64 ./out/build/linux-amd64-relwithdebinfo/GoldenEye --game_data_root=$PWD/assets --log_level info --log_file=<path> --ge_spike_log=true`.

- [ ] **Step 1: Cold-cache run with spike logging**

```bash
cd /home/keith/Projects/GoldenEye-Recomp
SCRATCH=/tmp/claude-1000/-home-keith-Projects-GoldenEye-Recomp/e13e463a-cbdf-4b94-a278-02419ca85208/scratchpad
rm -rf ~/.local/share/ge/cache/shaders   # cold: forces translation + compiles at boot
LD_LIBRARY_PATH=../GoldenEye-Recomp-rexglue/out/linux-amd64 \
  ./out/build/linux-amd64-relwithdebinfo/GoldenEye --game_data_root=$PWD/assets \
  --log_level info --log_file=$SCRATCH/ge-attr-smoke.log --ge_spike_log=true &
GEPID=$!
# poll up to 90s for a GESPIKE line carrying the new fields
deadline=$((SECONDS+90))
until grep -q "strans=" $SCRATCH/ge-attr-smoke.log 2>/dev/null; do
  kill -0 $GEPID 2>/dev/null || { echo EXITED; break; }
  [ $SECONDS -ge $deadline ] && { echo TIMEOUT; break; }
  sleep 2
done
grep "GESPIKE" $SCRATCH/ge-attr-smoke.log | head -5
kill -9 $GEPID 2>/dev/null
```

Expected: GESPIKE lines end with `... strans=<N>us pcomp=<N>us texup=<N>us gio=<N>us nshad=<N> npipe=<N>`; on a cold cache at boot, at least one line has `strans` or `gio` clearly > 0. (If the boot happens to produce no spike, relaunch once — boot on a cold cache reliably spikes.)

- [ ] **Step 2: Report end-to-end**

```bash
python3 scripts/perf_report.py $SCRATCH/ge-attr-smoke.log
```
Expected: no traceback; spike clustering runs; if a first-use stage dominated a spike it appears by its STAGES label.

- [ ] **Step 3: Overhead sanity (spec success criterion)**

While the game from Step 1 is still running (or in one fresh warm-cache run), let it idle 60s at the menu, then:

```bash
grep "GEFPS avg=" $SCRATCH/ge-attr-smoke.log | tail -3
```
Expected: avg fps in the same band as recent pre-change sessions (~52-60 at the menu on this desktop; the counters only tick on first-use events and file IO, so any drop beyond noise means something is timing a hot path — investigate before proceeding).

- [ ] **Step 4: Record pass/fail in the task report** (no commit — verification only)

---

### Task 6: Thor capture (warm/cold) + findings note — THE PHASE DELIVERABLE

**Files:**
- Create: `docs/superpowers/2026-07-XX-first-shot-hitch-attribution.md` (name it with the actual capture date)

**Interfaces:**
- Consumes: everything above, installed on the Thor (`adb -s 192.168.1.182:41285`; debug build is already the installed flavor). `ge_spike_log` is forced true on Android. Device log: `/sdcard/Android/data/com.sunjaycy.goldeneye/files/ge.log` (truncates per run — pull between runs).

**NOTE FOR THE CONTROLLER:** the gameplay portion (boot to Dam, shoot ≥4 distinct fresh enemy/prop types) requires the USER at the device. Agents handle build/install/cache-manipulation/log-pull/analysis; coordinate the play sessions with the user.

- [ ] **Step 1: Build + install**

```bash
cd /home/keith/Projects/GoldenEye-Recomp/android
./gradlew :app:installDebug -PrexSdkDir=/home/keith/Projects/GoldenEye-Recomp-rexglue
```

- [ ] **Step 2: WARM run (pipeline cache present)** — user plays: fresh boot → Dam → shoot ≥4 distinct fresh enemy/prop types → quit. Then:

```bash
adb -s 192.168.1.182:41285 pull /sdcard/Android/data/com.sunjaycy.goldeneye/files/ge.log out/attr-warm-ge.log
python3 scripts/perf_report.py out/attr-warm-ge.log
```

- [ ] **Step 3: COLD run** — delete the device pipeline cache, then same session shape:

```bash
adb -s 192.168.1.182:41285 shell "rm -f /sdcard/Android/data/com.sunjaycy.goldeneye/files/cache/shaders/shareable/*.vk.pcache"
# user plays the same session shape, then:
adb -s 192.168.1.182:41285 pull /sdcard/Android/data/com.sunjaycy.goldeneye/files/ge.log out/attr-cold-ge.log
python3 scripts/perf_report.py out/attr-cold-ge.log
```

- [ ] **Step 4: Write the findings note**

`docs/superpowers/<capture-date>-first-shot-hitch-attribution.md` must state, for each first-shot spike found (correlate GESPIKE timestamps with `_hits` VFS warnings in the same log): the µs split `gio` / `strans` / `pcomp` / `texup` / unattributed-net-`cpexec`, warm vs cold comparison (what did item 1's pipeline cache buy?), and a one-paragraph recommendation for Phase 2B (prewarm target: file IO vs shader/pipeline vs texture — or "hitch already acceptable").

- [ ] **Step 5: Commit the findings note (game branch)**

```bash
cd /home/keith/Projects/GoldenEye-Recomp
git add docs/superpowers/*first-shot-hitch-attribution.md
git commit -m "docs(perf): first-shot hitch attribution findings (phase 2A capture)"
```

---

### Task 7: Finish the paired branches

- [ ] **Step 1: Review both diffs**

```bash
git -C /home/keith/Projects/GoldenEye-Recomp-rexglue diff main...feat/gespike-attribution
git -C /home/keith/Projects/GoldenEye-Recomp diff develop...feat/gespike-attribution
```

- [ ] **Step 2: Merge TOGETHER (paired)**

```bash
cd /home/keith/Projects/GoldenEye-Recomp-rexglue && git checkout main && \
  git merge --no-ff feat/gespike-attribution -m "merge: first-use attribution counters" && \
  git branch -d feat/gespike-attribution
cd /home/keith/Projects/GoldenEye-Recomp && git checkout develop && \
  git merge --no-ff feat/gespike-attribution -m "merge: GESPIKE attribution fields + report tooling" && \
  git branch -d feat/gespike-attribution
```

- [ ] **Step 3: Commit spec + plan docs on develop**

```bash
cd /home/keith/Projects/GoldenEye-Recomp
git add docs/superpowers/specs/2026-07-10-gespike-attribution-counters-design.md \
        docs/superpowers/plans/2026-07-11-gespike-attribution-counters.md
git commit -m "docs: GESPIKE attribution counters spec + plan (perf deep-dive item 2A)"
```
