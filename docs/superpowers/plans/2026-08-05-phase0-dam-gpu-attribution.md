# Phase-0 Dam GPU Attribution Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Determine whether Dam's ~13.5ms resolution-independent GPU cost on the Ayn Thor is emulation-shaped (EDRAM transfers/resolves) or geometry-bound, so the native-renderer decision rests on measurement instead of inference.

**Architecture:** Merge the existing deterministic Dam benchmark branch, close the two instrumentation gaps that stop it answering the question (no GPU-time field in GEBENCH; no way to change init-only cvars on Android without a rebuild), teach the log analyzer to read the new output, then run a pre-registered A/B protocol on device. No renderer work. No SDK changes.

**Tech Stack:** C++20 (game code, header-only `GeApp`), CMake presets, Gradle/NDK for Android, Python 3 stdlib only for log analysis.

## Global Constraints

- **Build config for all measurement runs: RelWithDebInfo, never Release.** Perf counters are compile-gated on `REXGLUE_ENABLE_PERF_COUNTERS`, defined for every config *except* Release (SDK `CMakeLists.txt:33`). In Release, `kGpuFrameUs`/`kDrawCalls` read zero. Android's Gradle debug variant maps to CMake RelWithDebInfo — `:app:installDebug` is correct.
- **No SDK changes.** Phase 0 is game-side plus log analysis only. If a task appears to need an SDK edit, stop and escalate.
- **Android specifics:** `./gradlew :app:installDebug -PrexSdkDir=/home/keith/Projects/GoldenEye-Recomp-rexglue` — the absolute `-PrexSdkDir` is required, `gradle.properties` resolves it wrong. Install downgrade needs `adb install -r -d`. Logs land at `/sdcard/Android/data/com.sunjaycy.goldeneye/files/{ge.log,stderr.txt}` and `ge.log` truncates per run.
- **Linux build/run:** `cmake --build --preset linux-amd64-relwithdebinfo --target ge`; run with `LD_LIBRARY_PATH=../GoldenEye-Recomp-rexglue/out/linux-amd64` and `--game_data_root=$PWD/assets`. Cvars are CLI flags on desktop (`--ge_fps_log=true`).
- **Python: stdlib only.** `scripts/perf_report.py` has no dependencies and the repo has no test framework — keep both true.
- **Decision threshold (pre-registered, do not renegotiate mid-run):** emulation-shaped iff the `fbo`↔`fsi` median-GPU-time delta is ≥2.7ms (20% of 13.5ms). EDRAM counts corroborate but cannot carry the verdict alone. If `fsi` is unavailable on the device, the verdict is *inconclusive*.

**Spec:** `docs/superpowers/specs/2026-08-05-phase0-dam-gpu-attribution-design.md`

---

## File Structure

| File | Responsibility | Task |
|---|---|---|
| `src/ge_replay.cpp` | Replay state machine + GEBENCH accumulation/reporting. Gains per-frame GPU and draw-call sampling. | 2 |
| `src/ge_app.h` | `GeApp` boot configuration. Gains the cvar-override file reader. | 3 |
| `scripts/perf_report.py` | ge.log/CSV analysis. Gains GEBENCH + EDRAM parsers and a `--selftest` mode. | 4 |
| `docs/superpowers/reports/2026-08-05-phase0-dam-verdict.md` | The deliverable: numbers, verdict, go/no-go. | 5 |

---

### Task 1: Merge the input-replay benchmark onto develop

The deterministic Dam benchmark already exists on `feat/input-replay` (`677d21b`, 9 commits, 927 LOC) and has been verified to merge cleanly — only `src/ge_hooks.cpp` is touched by both branches.

**Files:**
- Modify: `src/ge_hooks.cpp`, `src/ge_hooks.h`, `src/ge_app.h`, `CMakeLists.txt` (all via merge)
- Create (via merge): `src/ge_replay.cpp`, `src/ge_replay.h`, `bench/dam-walk.gerp`, `bench/dam.macro`

**Interfaces:**
- Consumes: nothing (first task)
- Produces: `ge::ReplayInit(std::filesystem::path user_data_root, std::function<void()> quit_requester)` declared in `src/ge_replay.h`; cvars `ge_replay_play`, `ge_replay_macro`, `ge_replay_record`, `ge_replay_probe`, `ge_replay_selftest`, `ge_bench_exit`; the `GEBENCH` log line

- [ ] **Step 1: Confirm the merge is still clean before touching anything**

```bash
cd /home/keith/Projects/GoldenEye-Recomp
git checkout develop
git merge-tree --write-tree develop feat/input-replay >/dev/null && echo "CLEAN" || echo "CONFLICTS - stop and report"
```

Expected: `CLEAN`. If it reports conflicts, stop — `develop` has moved since this plan was written and the merge needs a human decision.

- [ ] **Step 2: Merge**

```bash
git merge --no-ff feat/input-replay -m "merge: input record/replay + GEBENCH harness for Phase-0 attribution"
```

- [ ] **Step 3: Build**

```bash
cmake --build --preset linux-amd64-relwithdebinfo --target ge 2>&1 | tail -20
```

Expected: build succeeds. `src/ge_replay.cpp` must appear in the compiled sources — if CMake does not pick it up, confirm the merge brought the `CMakeLists.txt` hunk that adds it.

- [ ] **Step 4: Verify the harness self-test passes**

The branch ships a record→replay loop-back self-test. This is the repo's established verification idiom (there is no unit-test framework).

```bash
LD_LIBRARY_PATH=../GoldenEye-Recomp-rexglue/out/linux-amd64 \
  ./out/build/linux-amd64-relwithdebinfo/GoldenEye --game_data_root=$PWD/assets \
  --ge_replay_selftest=true --log_level=info 2>&1 | grep -i "selftest"
```

Expected: a self-test verdict line reporting success. If it reports failure, stop — the harness is broken and every downstream measurement would be worthless.

- [ ] **Step 5: Commit**

The merge commit from Step 2 is the commit. Verify it landed:

```bash
git log --oneline -1
git ls-files src/ge_replay.cpp bench/dam-walk.gerp
```

Expected: merge commit at HEAD, both files tracked.

---

### Task 2: Add GPU-time and draw-call sampling to GEBENCH

`BenchOnPoll()` currently accumulates four counters — `kShaderTranslateUs`, `kPipelineCompileUs`, `kTextureUploadUs`, `kGuestFileIoUs` — and no GPU time. `kGpuFrameUs` surfaces only via `GESPIKE`, which fires only on frames exceeding 2× the rolling median, i.e. never on the steady-state frames under attribution. Without this task the benchmark cannot answer the spec's question.

Samples are retained per-frame (not summed) because the **median** is what compares against the 13.5ms figure; a mean is skewed by exactly the hitches the analysis excludes.

**Files:**
- Modify: `src/ge_replay.cpp` (the `BenchState` struct, `BenchOnPoll()`, `BenchFinish()`)

**Interfaces:**
- Consumes: `ge::ReplayInit` and the GEBENCH line from Task 1; `rex::perf::GetSnapshotCounter(rex::perf::CounterId)` returning `int64_t` (SDK `include/rex/perf/counter.h:94`); `rex::perf::CounterId::kGpuFrameUs` (`:61`) and `::kDrawCalls` (`:29`)
- Produces: the extended `GEBENCH` line with fields `gpu_med_ms`, `gpu_p95_ms`, `draws_med` inserted after `hitch=` and before `strans_ms=`. Task 4's parser matches this exact ordering.

- [ ] **Step 1: Extend `BenchState` with per-frame sample vectors**

In `src/ge_replay.cpp`, the struct currently reads:

```cpp
struct BenchState {
  std::vector<int64_t> poll_ts_us;
  int64_t strans_us = 0;
  int64_t pcomp_us = 0;
  int64_t texup_us = 0;
  int64_t gio_us = 0;
  uint64_t last_refresh = 0;  // rex_ge_guest_refresh_count() as of the last poll
};
```

Replace it with:

```cpp
struct BenchState {
  std::vector<int64_t> poll_ts_us;
  // Per-frame samples, not running totals: the attribution question (see
  // docs/superpowers/specs/2026-08-05-phase0-dam-gpu-attribution-design.md)
  // compares a *median* GPU frame time against a 13.5ms fixed cost, and a mean
  // would be dragged by the very hitches the analysis excludes.
  std::vector<int64_t> gpu_us;
  std::vector<int64_t> draws;
  int64_t strans_us = 0;
  int64_t pcomp_us = 0;
  int64_t texup_us = 0;
  int64_t gio_us = 0;
  uint64_t last_refresh = 0;  // rex_ge_guest_refresh_count() as of the last poll
};
```

- [ ] **Step 2: Sample the two counters in `BenchOnPoll()`**

Inside the existing guest-refresh-advanced guard in `BenchOnPoll()` — the same guard that protects the four stage accumulators from double-counting when the guest polls more than once per frame — append the two samples. The block currently reads:

```cpp
  uint64_t refresh = rex_ge_guest_refresh_count();
  if (refresh != g_bench.last_refresh) {
    g_bench.last_refresh = refresh;
    g_bench.strans_us += rex::perf::GetSnapshotCounter(rex::perf::CounterId::kShaderTranslateUs);
    g_bench.pcomp_us += rex::perf::GetSnapshotCounter(rex::perf::CounterId::kPipelineCompileUs);
    g_bench.texup_us += rex::perf::GetSnapshotCounter(rex::perf::CounterId::kTextureUploadUs);
    g_bench.gio_us += rex::perf::GetSnapshotCounter(rex::perf::CounterId::kGuestFileIoUs);
  }
```

Add these two lines at the end of the `if` body:

```cpp
    g_bench.gpu_us.push_back(
        rex::perf::GetSnapshotCounter(rex::perf::CounterId::kGpuFrameUs));
    g_bench.draws.push_back(
        rex::perf::GetSnapshotCounter(rex::perf::CounterId::kDrawCalls));
```

- [ ] **Step 3: Report medians in `BenchFinish()`**

`BenchFinish()` already sorts frame intervals into `ft` and computes `p50`/`p99`/`worst`. Add a percentile helper immediately before the `REXKRNL_INFO` call — it takes the vector by value because it sorts:

```cpp
  // Percentile over a copy (sorts in place). p is 0..100.
  auto pctl = [](std::vector<int64_t> v, int p) -> double {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    size_t idx = size_t(double(v.size() - 1) * (double(p) / 100.0));
    return double(v[idx]);
  };
  const double gpu_med_ms = pctl(g_bench.gpu_us, 50) / 1000.0;
  const double gpu_p95_ms = pctl(g_bench.gpu_us, 95) / 1000.0;
  const double draws_med = pctl(g_bench.draws, 50);
```

Then replace the existing `REXKRNL_INFO` call with:

```cpp
  REXKRNL_INFO(
      "GEBENCH frames={} dur={:.1f}s avg={:.1f} low1={:.1f} worst={:.1f} hitch={} "
      "gpu_med_ms={:.2f} gpu_p95_ms={:.2f} draws_med={:.0f} "
      "strans_ms={:.1f} pcomp_ms={:.1f} texup_ms={:.1f} gio_ms={:.1f}",
      ft.size(), dur_s, fps(p50), fps(p99), fps(worst), hitches,
      gpu_med_ms, gpu_p95_ms, draws_med,
      g_bench.strans_us / 1000.0, g_bench.pcomp_us / 1000.0, g_bench.texup_us / 1000.0,
      g_bench.gio_us / 1000.0);
```

`<algorithm>` and `<vector>` are already included by this file (it calls `std::sort` and `std::count_if` already); do not add includes.

- [ ] **Step 4: Build**

```bash
cmake --build --preset linux-amd64-relwithdebinfo --target ge 2>&1 | tail -20
```

Expected: success.

- [ ] **Step 5: Verify the new fields appear with non-zero values**

A zero `gpu_med_ms` means either GPU timestamps are off or the build lacks perf counters — both invalidate every later measurement, so this check is the gate.

```bash
LD_LIBRARY_PATH=../GoldenEye-Recomp-rexglue/out/linux-amd64 \
  ./out/build/linux-amd64-relwithdebinfo/GoldenEye --game_data_root=$PWD/assets \
  --ge_replay_macro=bench/dam.macro --ge_replay_play=bench/dam-walk.gerp \
  --ge_gpu_timestamps=true --ge_bench_exit=true --log_level=info \
  --log_file=/tmp/claude-1000/-home-keith-Projects-GoldenEye-Recomp/bef81402-f292-48e5-8003-ea1a62aef9f9/scratchpad/ge-task2.log
grep "GEBENCH" /tmp/claude-1000/-home-keith-Projects-GoldenEye-Recomp/bef81402-f292-48e5-8003-ea1a62aef9f9/scratchpad/ge-task2.log
```

Expected: one `GEBENCH` line containing `gpu_med_ms=`, `gpu_p95_ms=`, `draws_med=`, with `gpu_med_ms` > 0 and `draws_med` > 0.

If `gpu_med_ms=0.00`: confirm the build is RelWithDebInfo (not Release) and that `ge_gpu_timestamps` is on. Report rather than working around it.

- [ ] **Step 6: Commit**

```bash
git add src/ge_replay.cpp
git commit -m "feat(bench): sample per-frame GPU time and draw calls into GEBENCH

GEBENCH reported no GPU time -- kGpuFrameUs surfaced only through GESPIKE,
which fires only on >2x-median frames, i.e. never on the steady-state frames
Phase-0 attribution needs. Samples are retained per-frame so the line can
report a median; a mean would be skewed by the excluded hitches."
```

---

### Task 3: Read cvar overrides from a pushable file

Android reads no config file and takes no CLI; cvars are hardcoded `SetFlagByName` calls in `GeApp::OnConfigurePaths` (`src/ge_app.h:56-107`). `render_target_path_vulkan` is init-only. Without this, the spec's three-config A/B costs three rebuild-and-install cycles *per repetition* — nine for the full protocol.

**Files:**
- Modify: `src/ge_app.h` (add the reader as a private static member of `GeApp`; call it at the end of `OnConfigurePaths`)

**Interfaces:**
- Consumes: `rex::cvar::SetFlagByName(std::string_view name, std::string_view value)` returning `bool` (SDK `include/rex/cvar.h:165`); `rex::PathConfig::user_data_root` (SDK `include/rex/rex_app.h:44`)
- Produces: cvar override file support at `<user_data_root>/ge_cvars.txt`, format `name=value` per line, `#` comments; log lines prefixed `GECVAR`

- [ ] **Step 1: Add the required includes**

`src/ge_app.h` currently includes `<functional>` and `<string>` from the standard library. Add three more alongside them, and the logging header alongside the other `rex/` includes:

```cpp
#include <rex/logging/macros.h>
```

```cpp
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
```

- [ ] **Step 2: Add the reader as a private static member of `GeApp`**

Place it in the class's private section:

```cpp
  // Apply "name=value" cvar overrides from a text file, if one exists.
  //
  // Android reads no config file and takes no CLI, so cvars are otherwise
  // hardcoded in OnConfigurePaths below -- and init-only cvars like
  // render_target_path_vulkan then cost a full rebuild + reinstall to change.
  // This lets `adb push` swap a config between runs instead. Applied last so it
  // beats the hardcoded defaults; on desktop, CLI flags still win as usual.
  //
  // Format: one name=value per line. '#' starts a comment. Blank lines and
  // lines without '=' are skipped. Unknown cvar names are logged and ignored.
  static void ApplyCvarOverrides(const std::filesystem::path& file) {
    std::error_code ec;
    if (!std::filesystem::exists(file, ec) || ec) {
      return;
    }
    std::ifstream in(file);
    if (!in) {
      REXKRNL_WARN("GECVAR override file exists but could not be opened: {}", file.string());
      return;
    }
    auto trim = [](std::string& s) {
      const char* ws = " \t\r\n";
      const auto b = s.find_first_not_of(ws);
      if (b == std::string::npos) {
        s.clear();
        return;
      }
      s = s.substr(b, s.find_last_not_of(ws) - b + 1);
    };
    std::string line;
    int applied = 0;
    while (std::getline(in, line)) {
      const auto hash = line.find('#');
      if (hash != std::string::npos) {
        line.erase(hash);
      }
      const auto eq = line.find('=');
      if (eq == std::string::npos) {
        continue;
      }
      std::string name = line.substr(0, eq);
      std::string value = line.substr(eq + 1);
      trim(name);
      trim(value);
      if (name.empty()) {
        continue;
      }
      if (rex::cvar::SetFlagByName(name, value)) {
        REXKRNL_INFO("GECVAR override applied: {}={}", name, value);
        ++applied;
      } else {
        REXKRNL_WARN("GECVAR override rejected (unknown cvar or bad value): {}={}", name, value);
      }
    }
    REXKRNL_INFO("GECVAR applied {} override(s) from {}", applied, file.string());
  }
```

- [ ] **Step 3: Call it at the very end of `OnConfigurePaths`**

`OnConfigurePaths` currently opens with `(void)paths;` and ends after the `#endif` closing the `__ANDROID__` block. Remove the `(void)paths;` line (the parameter is now used) and add this as the last statement of the method, after the `#endif`:

```cpp
    // Last, so a pushed file can override every default set above. See
    // ApplyCvarOverrides for why this exists.
    ApplyCvarOverrides(paths.user_data_root / "ge_cvars.txt");
```

- [ ] **Step 4: Build**

```bash
cmake --build --preset linux-amd64-relwithdebinfo --target ge 2>&1 | tail -20
```

Expected: success. A compile error about `paths.user_data_root` being unpopulated is not possible (it is a plain struct member), but see Step 5 for whether it holds the value we need at this point in startup.

- [ ] **Step 5: Verify the path resolves and overrides apply**

`OnConfigurePaths` runs during the SDK's `SetupEnvironment` phase, so `paths.user_data_root` may or may not be populated yet. This step establishes which, empirically, rather than assuming.

```bash
SCRATCH=/tmp/claude-1000/-home-keith-Projects-GoldenEye-Recomp/bef81402-f292-48e5-8003-ea1a62aef9f9/scratchpad
LD_LIBRARY_PATH=../GoldenEye-Recomp-rexglue/out/linux-amd64 \
  ./out/build/linux-amd64-relwithdebinfo/GoldenEye --game_data_root=$PWD/assets --log_level=info \
  --log_file=$SCRATCH/ge-task3-nofile.log &
sleep 25 && kill %1 2>/dev/null
grep "GECVAR" $SCRATCH/ge-task3-nofile.log
```

Expected: `GECVAR applied 0 override(s) from <path>`. **Record that path.** If it is empty or obviously wrong (e.g. `/ge_cvars.txt`), `user_data_root` is not yet populated at this point — in that case switch the call to use the SDK's `user_data_root` cvar (`REXCVAR_DECLARE(std::string, user_data_root)`, SDK `include/rex/runtime.h:35`) and re-run this step.

Then create an override file at the recorded path and confirm it applies:

```bash
printf '# phase-0 test\nge_fps_log=true\nrender_target_path_vulkan=fbo\n' > <recorded-path>/ge_cvars.txt
LD_LIBRARY_PATH=../GoldenEye-Recomp-rexglue/out/linux-amd64 \
  ./out/build/linux-amd64-relwithdebinfo/GoldenEye --game_data_root=$PWD/assets --log_level=info \
  --log_file=$SCRATCH/ge-task3-withfile.log &
sleep 25 && kill %1 2>/dev/null
grep "GECVAR\|render target" $SCRATCH/ge-task3-withfile.log
```

Expected: two `GECVAR override applied` lines, `GECVAR applied 2 override(s)`, and the SDK logging the FBO render-target path selection.

- [ ] **Step 6: Confirm a malformed file cannot break boot**

```bash
printf 'garbage line with no equals\n=missingname\nnot_a_real_cvar=7\n\n# comment only\n' > <recorded-path>/ge_cvars.txt
LD_LIBRARY_PATH=../GoldenEye-Recomp-rexglue/out/linux-amd64 \
  ./out/build/linux-amd64-relwithdebinfo/GoldenEye --game_data_root=$PWD/assets --log_level=info \
  --log_file=$SCRATCH/ge-task3-malformed.log &
sleep 25 && kill %1 2>/dev/null
grep "GECVAR" $SCRATCH/ge-task3-malformed.log
rm <recorded-path>/ge_cvars.txt
```

Expected: the game boots normally; one `GECVAR override rejected` line for `not_a_real_cvar`; `GECVAR applied 0 override(s)`. The garbage and `=missingname` lines are silently skipped.

- [ ] **Step 7: Commit**

```bash
git add src/ge_app.h
git commit -m "feat(app): apply cvar overrides from a pushable ge_cvars.txt

Android reads no config file and takes no CLI, so init-only cvars such as
render_target_path_vulkan previously cost a rebuild + reinstall to change --
nine cycles for the Phase-0 3-config x 3-rep protocol. Applied last in
OnConfigurePaths so a pushed file beats the hardcoded defaults."
```

---

### Task 4: Teach perf_report.py to read GEBENCH and EDRAM lines

`scripts/perf_report.py` parses GEFPS, GESHOWN, and GESPIKE. It knows nothing about GEBENCH (new field layout from Task 2) or the SDK's EDRAM round-trip lines, which are the corroborating discriminator.

The repo has no test framework and `perf_report.py` has no dependencies. Both stay true: verification is a `--selftest` mode asserting the parsers against embedded sample lines, matching the repo's existing self-test-via-flag idiom (`ge_replay_selftest`).

**Files:**
- Modify: `scripts/perf_report.py`

**Interfaces:**
- Consumes: the GEBENCH line format produced by Task 2; the SDK's EDRAM line from `render_target_cache.cpp:129`
- Produces: `--selftest` flag; GEBENCH and EDRAM sections in `analyze_log` output

- [ ] **Step 1: Write the failing self-test first**

Add near the bottom of `scripts/perf_report.py`, above `main`:

```python
# Sample lines for --selftest. Copied verbatim from real output: the GEBENCH
# format is produced by BenchFinish() in src/ge_replay.cpp, the EDRAM line by
# the SDK's render_target_cache.cpp. If either format changes, this fails loudly
# rather than silently parsing nothing.
SELFTEST_GEBENCH = (
    "GEBENCH frames=3601 dur=60.0s avg=59.9 low1=41.2 worst=22.0 hitch=3 "
    "gpu_med_ms=13.48 gpu_p95_ms=19.02 draws_med=412 "
    "strans_ms=0.0 pcomp_ms=12.5 texup_ms=3.2 gio_ms=0.4")
SELFTEST_EDRAM = (
    "EDRAM round-trips frame 5123: 47 transfer passes, 188 transfer draws, "
    "12 host-depth stores across 61 reconfigs (14 skipped as no-op)")


def selftest():
    failures = []

    m = GEBENCH_RE.search(SELFTEST_GEBENCH)
    if not m:
        failures.append("GEBENCH_RE did not match the sample line")
    else:
        g = m.groupdict()
        for key, want in (("frames", "3601"), ("gpu_med", "13.48"),
                          ("gpu_p95", "19.02"), ("draws_med", "412"),
                          ("pcomp", "12.5")):
            if g[key] != want:
                failures.append(f"GEBENCH {key}: got {g[key]!r}, want {want!r}")

    m = EDRAM_RE.search(SELFTEST_EDRAM)
    if not m:
        failures.append("EDRAM_RE did not match the sample line")
    else:
        g = m.groupdict()
        for key, want in (("frame", "5123"), ("passes", "47"), ("draws", "188"),
                          ("depth", "12"), ("reconfigs", "61"), ("skipped", "14")):
            if g[key] != want:
                failures.append(f"EDRAM {key}: got {g[key]!r}, want {want!r}")

    if failures:
        for f in failures:
            print(f"FAIL: {f}")
        return 1
    print("selftest: OK (GEBENCH + EDRAM parsers)")
    return 0
```

And wire it into `main`, which currently starts:

```python
def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 1
```

Change that to:

```python
def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 1
    if argv[1] == "--selftest":
        return selftest()
```

- [ ] **Step 2: Run it to confirm it fails**

```bash
cd /home/keith/Projects/GoldenEye-Recomp
python3 scripts/perf_report.py --selftest
```

Expected: `NameError: name 'GEBENCH_RE' is not defined` — the regexes do not exist yet.

- [ ] **Step 3: Add the two regexes**

Place them alongside the existing `GESPIKE_RE` / `GEFPS_RE` / `GESHOWN_RE` definitions:

```python
GEBENCH_RE = re.compile(
    r"GEBENCH frames=(?P<frames>\d+) dur=(?P<dur>[\d.]+)s avg=(?P<avg>[\d.]+) "
    r"low1=(?P<low1>[\d.]+) worst=(?P<worst>[\d.]+) hitch=(?P<hitch>\d+) "
    r"gpu_med_ms=(?P<gpu_med>[\d.]+) gpu_p95_ms=(?P<gpu_p95>[\d.]+) "
    r"draws_med=(?P<draws_med>[\d.]+) "
    r"strans_ms=(?P<strans>[\d.]+) pcomp_ms=(?P<pcomp>[\d.]+) "
    r"texup_ms=(?P<texup>[\d.]+) gio_ms=(?P<gio>[\d.]+)")

EDRAM_RE = re.compile(
    r"EDRAM round-trips frame (?P<frame>\d+): (?P<passes>\d+) transfer passes, "
    r"(?P<draws>\d+) transfer draws, (?P<depth>\d+) host-depth stores "
    r"across (?P<reconfigs>\d+) reconfigs \((?P<skipped>\d+) skipped as no-op\)")
```

- [ ] **Step 4: Run the self-test to confirm it passes**

```bash
python3 scripts/perf_report.py --selftest
```

Expected: `selftest: OK (GEBENCH + EDRAM parsers)`

- [ ] **Step 5: Report both in `analyze_log`**

In `analyze_log`, add two collectors beside the existing ones. Change:

```python
    gefps, geshown, gespikes = [], [], []
```

to:

```python
    gefps, geshown, gespikes, gebench, edram = [], [], [], [], []
```

Then inside the per-line loop, before the `GEFPS_RE` check (GEBENCH appears once per run; checking it first costs nothing and keeps the cheap matches early):

```python
            m = GEBENCH_RE.search(line)
            if m:
                gebench.append({k: float(v) for k, v in m.groupdict().items()})
                continue
            m = EDRAM_RE.search(line)
            if m:
                edram.append({k: float(v) for k, v in m.groupdict().items()})
                continue
```

Then add reporting after the existing `if gefps:` block:

```python
    if gebench:
        b = gebench[-1]
        print(f"Benchmark: {int(b['frames'])} frames in {b['dur']:.1f}s, "
              f"avg {b['avg']:.1f}fps, 1%-low {b['low1']:.1f}, "
              f"hitches {int(b['hitch'])}")
        print(f"  GPU: median {b['gpu_med']:.2f}ms, p95 {b['gpu_p95']:.2f}ms, "
              f"median draws/frame {int(b['draws_med'])}")
        print(f"  CPU stages: pcomp {b['pcomp']:.1f}ms, texup {b['texup']:.1f}ms, "
              f"strans {b['strans']:.1f}ms, gio {b['gio']:.1f}ms")
        if b["gpu_med"] == 0.0:
            print("  !! gpu_med_ms is 0 -- Release build (perf counters compiled "
                  "out) or ge_gpu_timestamps off. Measurement is INVALID.")
    if edram:
        passes = sorted(e["passes"] for e in edram)
        draws = sorted(e["draws"] for e in edram)
        depth = sorted(e["depth"] for e in edram)
        print(f"EDRAM round-trips over {len(edram)} frames: "
              f"median {pct(passes, 50):.0f} transfer passes/frame "
              f"(p95 {pct(passes, 95):.0f}), median {pct(draws, 50):.0f} transfer "
              f"draws, median {pct(depth, 50):.0f} host-depth stores")
```

- [ ] **Step 6: Verify against a real log**

```bash
python3 scripts/perf_report.py /tmp/claude-1000/-home-keith-Projects-GoldenEye-Recomp/bef81402-f292-48e5-8003-ea1a62aef9f9/scratchpad/ge-task2.log
```

Expected: a `Benchmark:` section with a non-zero GPU median. The EDRAM section will be absent (that log ran without `vulkan_edram_roundtrip_stats`) — that is correct behavior, not a failure.

- [ ] **Step 7: Commit**

```bash
git add scripts/perf_report.py
git commit -m "feat(perf): parse GEBENCH and EDRAM round-trip lines

Adds the two parsers Phase-0 attribution reads, plus a --selftest mode
asserting both against verbatim sample lines so a format drift fails loudly
instead of silently parsing nothing. Stdlib only; no test framework added."
```

---

### Task 5: Run the protocol on the Thor and write the verdict

Everything before this was tooling. This task produces the actual Phase-0 deliverable.

**Files:**
- Create: `docs/superpowers/reports/2026-08-05-phase0-dam-verdict.md`
- Create (transient, on device): `/sdcard/Android/data/com.sunjaycy.goldeneye/files/ge_cvars.txt`

**Interfaces:**
- Consumes: everything from Tasks 1–4
- Produces: the go / no-go / inconclusive verdict gating all conditional work in spec §7

- [ ] **Step 1: Build and install the RelWithDebInfo APK**

```bash
cd /home/keith/Projects/GoldenEye-Recomp/android
./gradlew :app:installDebug -PrexSdkDir=/home/keith/Projects/GoldenEye-Recomp-rexglue
```

The Gradle debug variant maps to CMake RelWithDebInfo, so perf counters are compiled in. If install fails on version downgrade, use `adb install -r -d <apk>`.

- [ ] **Step 2: Confirm the override file works on device before spending any run time**

```bash
adb shell 'printf "ge_fps_log=true\n" > /sdcard/Android/data/com.sunjaycy.goldeneye/files/ge_cvars.txt'
adb shell am force-stop com.sunjaycy.goldeneye
adb shell monkey -p com.sunjaycy.goldeneye -c android.intent.category.LAUNCHER 1
sleep 45
adb shell 'grep GECVAR /sdcard/Android/data/com.sunjaycy.goldeneye/files/ge.log'
```

Expected: `GECVAR override applied: ge_fps_log=true` and `GECVAR applied 1 override(s)`.

If the file is not found, the path `paths.user_data_root` resolves to on Android differs from the log directory — read the path out of the `GECVAR applied 0 override(s) from <path>` line and use that instead. Do not proceed until an override demonstrably applies; the entire protocol depends on it.

- [ ] **Step 3: Establish the run recipe**

One run = push a config, force-stop, launch, wait for the replay to finish and `ge_bench_exit` to quit, pull `ge.log` (it truncates per run, so pull before the next launch).

```bash
run_config() {  # $1 = config name, $2 = extra cvar lines, $3 = rep number
  adb shell "printf 'ge_fps_log=true\nge_spike_log=true\nge_gpu_timestamps=true\nvulkan_edram_roundtrip_stats=true\nge_replay_macro=bench/dam.macro\nge_replay_play=bench/dam-walk.gerp\nge_bench_exit=true\n$2' > /sdcard/Android/data/com.sunjaycy.goldeneye/files/ge_cvars.txt"
  adb shell am force-stop com.sunjaycy.goldeneye
  adb shell monkey -p com.sunjaycy.goldeneye -c android.intent.category.LAUNCHER 1
  # Wait for the app to exit on its own (ge_bench_exit). Poll rather than sleep.
  for i in $(seq 1 90); do
    sleep 5
    adb shell pidof com.sunjaycy.goldeneye >/dev/null 2>&1 || break
  done
  adb pull /sdcard/Android/data/com.sunjaycy.goldeneye/files/ge.log "phase0-$1-rep$3.log"
}
```

Note: `bench/dam-walk.gerp` and `bench/dam.macro` must be reachable on device. If the replay does not start, check whether the APK packages `bench/` — if it does not, push the two files to the app's files directory and point the cvars at absolute paths there.

- [ ] **Step 4: Run the protocol, interleaved**

Run order is interleaved by design: paint time is documented degrading 4.8→36ms over minutes, so running all of one config then the next would confound config with device temperature.

```bash
for rep in 1 2 3; do
  run_config default ""                                        $rep
  run_config fbo     "render_target_path_vulkan=fbo\n"         $rep
  run_config fsi     "render_target_path_vulkan=fsi\n"         $rep
done
run_config scale2 "resolution_scale=2\n" 1
```

Between reps, let the device idle a few minutes to shed heat. Record the wall-clock start time of each run.

- [ ] **Step 5: Check whether `fsi` was actually selected**

The path auto-falls back and logs the reason if the device cannot support it. If `fsi` silently became `fbo`, the primary discriminator is void.

```bash
grep -i "fragment shader interlock\|render target path\|switching to" phase0-fsi-rep1.log
```

Expected: confirmation the FSI path is active. If it fell back, the verdict is **inconclusive** per the spec — record which, and do not substitute a different discriminator.

- [ ] **Step 6: Analyze**

```bash
cd /home/keith/Projects/GoldenEye-Recomp
for f in phase0-*.log; do echo "### $f"; python3 scripts/perf_report.py "$f"; done
```

Watch for `gpu_med_ms is 0` warnings — any run showing that is invalid and must be rerun on a RelWithDebInfo build.

- [ ] **Step 7: Write the verdict**

Create `docs/superpowers/reports/2026-08-05-phase0-dam-verdict.md` containing, at minimum:

1. Whether the 13.5ms + 6.5ms/1× decomposition reproduced (from the `default` vs `scale2` runs).
2. A table of median GPU frame time per config, all three reps, with the spread.
3. Median EDRAM transfer passes / transfer draws / host-depth stores per frame on Dam.
4. The `fbo`↔`fsi` median delta, and whether it clears the pre-registered 2.7ms threshold.
5. One of three verdicts — **emulation-shaped**, **geometry-bound**, or **inconclusive** — and the resulting go / no-go / escalate for the conditional work in spec §7.
6. Any run discarded, and why (frame-count mismatch, thermal outlier, invalid counters).

State the verdict plainly even if it is no-go. A no-go is a successful Phase 0: it saves weeks of renderer work and redirects effort to the cheaper levers.

- [ ] **Step 8: Commit**

```bash
git add docs/superpowers/reports/2026-08-05-phase0-dam-verdict.md
git commit -m "docs(report): Phase-0 verdict on the Dam fixed GPU cost"
```

---

## Self-Review

**Spec coverage:**

| Spec section | Covered by |
|---|---|
| §2 primary discriminator (path A/B, ≥2.7ms) | Task 5 steps 4–7; threshold restated in Global Constraints |
| §2 secondary discriminator (EDRAM counts) | Task 4 (parser), Task 5 steps 4, 6 |
| §2 inconclusive-if-no-fsi | Task 5 step 5 |
| §2 baseline validation (1×→2×) | Task 5 step 4 (`scale2` run), step 7 item 1 |
| §4.1 GEBENCH GPU-time gap | Task 2 |
| §4.2 Android cvar override | Task 3 |
| §4.3 perf_report.py parsers | Task 4 |
| §5 protocol (configs, 3 reps, interleaved, capture, analysis) | Task 5 steps 3–6 |
| §5 RelWithDebInfo requirement | Global Constraints; Task 5 step 1; guard in Task 2 step 5 and Task 4 step 5 |
| §6 risks (macro retiming, thermal, fsi unavailable, determinism drift) | Task 5 steps 3–5, step 7 item 6 |
| §8 success criteria | Task 5 step 7, items 1–5 |

**Placeholder scan:** No TBD/TODO. Every code step carries the actual code. The one deliberately unresolved value is the `user_data_root` path in Task 3 steps 5–6, written as `<recorded-path>` — Step 5 establishes it empirically and states the fallback, because asserting a path I have not observed at that point in startup would be a guess.

**Type consistency:** `BenchState.gpu_us` / `.draws` are `std::vector<int64_t>`, matching `GetSnapshotCounter`'s `int64_t` return. GEBENCH field names (`gpu_med_ms`, `gpu_p95_ms`, `draws_med`) are identical in Task 2's format string, Task 4's `GEBENCH_RE` named groups (`gpu_med`, `gpu_p95`, `draws_med` — group names deliberately drop the `_ms` suffix, consistently across regex, selftest, and reporting), and Task 4's report block. `ApplyCvarOverrides` is declared and called under that exact name.
