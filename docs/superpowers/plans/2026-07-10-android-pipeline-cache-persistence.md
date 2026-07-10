# Android Vulkan Pipeline-Cache Persistence Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Persist the native `VkPipelineCache` blob to disk on a quiescence debounce so Android (which never runs clean shutdown) stops recompiling every Vulkan pipeline cold on each launch.

**Architecture:** SDK-only change in `VulkanPipelineCache`. Pipeline-creation sites stamp an atomic dirty flag + timestamp; the existing storage-write thread gains a timed wait that calls `StorePipelineDiskCache()` after 10s with no new creations. The store itself becomes atomic (tmp + rename) so a mid-write kill can't corrupt the seed blob. Spec: `docs/superpowers/specs/2026-07-10-android-pipeline-cache-persistence-design.md`.

**Tech Stack:** C++17, Vulkan, CMake preset `linux-amd64-relwithdebinfo`, Android gradle build.

## Global Constraints

- All code changes live in the SDK checkout `/home/keith/Projects/GoldenEye-Recomp-rexglue` (branch off `main`).
- The game repo is only touched for these docs — no game-side code changes.
- Do NOT use a git worktree for the SDK: the game build hard-references the sibling path `../GoldenEye-Recomp-rexglue` (`REXSDK_DIR`), so work on a branch in place.
- Quiescence window: **10 seconds**, compile-time constant (`kPipelineDiskCacheQuiescentMs = 10000`), no new cvar.
- Feature is implicitly gated by existing cvar `vulkan_persistent_pipeline_cache` (when off, `pipeline_disk_cache_path_` is empty and everything below no-ops).
- **Testing adaptation:** this SDK has no unit-test framework that can exercise Vulkan-device code; each task's test cycle is a build plus a concrete runtime verification with expected observable output (file on disk, greppable log line). Run them exactly as written.
- Linux cache dir: `~/.local/share/ge/cache/shaders/shareable/`. Thor cache dir: `/sdcard/Android/data/com.sunjaycy.goldeneye/files/cache/shaders/shareable/`.
- Android build: `cd /home/keith/Projects/GoldenEye-Recomp/android && ./gradlew :app:installDebug -PrexSdkDir=/home/keith/Projects/GoldenEye-Recomp-rexglue` (absolute `-PrexSdkDir` required). Thor adb serial: `192.168.1.182:41285` (verify with `adb devices` + `getprop ro.product.model` = "AYN Thor").

---

### Task 1: Atomic (tmp + rename) store with success log

**Files:**
- Modify: `/home/keith/Projects/GoldenEye-Recomp-rexglue/src/graphics/vulkan/pipeline_cache.cpp:381-410` (`StorePipelineDiskCache`)

**Interfaces:**
- Consumes: existing `pipeline_disk_cache_`, `pipeline_disk_cache_path_`, `rex::filesystem::OpenFile`, `rex::path_to_utf8`, `REXGPU_INFO/WARN`.
- Produces: `StorePipelineDiskCache()` (signature unchanged) that is kill-safe and logs `VulkanPipelineCache: Stored persistent VkPipelineCache to <path> (<N> bytes)` on success — Task 3/4 grep for this exact line.

- [ ] **Step 1: Create the SDK feature branch**

```bash
cd /home/keith/Projects/GoldenEye-Recomp-rexglue
git checkout main && git pull --ff-only 2>/dev/null; git checkout -b feat/pipeline-cache-persistence
```

- [ ] **Step 2: Replace the write tail of `StorePipelineDiskCache`**

In `src/graphics/vulkan/pipeline_cache.cpp`, replace the current tail (lines 402–410):

```cpp
  FILE* file = rex::filesystem::OpenFile(pipeline_disk_cache_path_, "wb");
  if (!file) {
    REXGPU_WARN("VulkanPipelineCache: Failed to open {} for writing the persistent pipeline cache",
                rex::path_to_utf8(pipeline_disk_cache_path_));
    return;
  }
  fwrite(data.data(), 1, data.size(), file);
  fclose(file);
```

with:

```cpp
  // Write to a temp file and rename over the final path so a mid-write kill
  // (normal app death on Android) can't corrupt the seed blob.
  std::filesystem::path tmp_path = pipeline_disk_cache_path_;
  tmp_path += ".tmp";
  FILE* file = rex::filesystem::OpenFile(tmp_path, "wb");
  if (!file) {
    REXGPU_WARN("VulkanPipelineCache: Failed to open {} for writing the persistent pipeline cache",
                rex::path_to_utf8(tmp_path));
    return;
  }
  size_t written = fwrite(data.data(), 1, data.size(), file);
  fclose(file);
  std::error_code ec;
  if (written != data.size()) {
    REXGPU_WARN("VulkanPipelineCache: Short write persisting the pipeline cache to {}",
                rex::path_to_utf8(tmp_path));
    std::filesystem::remove(tmp_path, ec);
    return;
  }
  std::filesystem::rename(tmp_path, pipeline_disk_cache_path_, ec);
  if (ec) {
    REXGPU_WARN("VulkanPipelineCache: Failed to rename {} over the persistent pipeline cache: {}",
                rex::path_to_utf8(tmp_path), ec.message());
    std::filesystem::remove(tmp_path, ec);
    return;
  }
  REXGPU_INFO("VulkanPipelineCache: Stored persistent VkPipelineCache to {} ({} bytes)",
              rex::path_to_utf8(pipeline_disk_cache_path_), data.size());
```

(`<filesystem>` and `<system_error>` come in via `rex/filesystem.h` / the header's `std::filesystem::path` usage; if the build complains about `std::error_code`, add `#include <system_error>` to the cpp's std includes block at lines 12–22.)

- [ ] **Step 3: Build**

```bash
cd /home/keith/Projects/GoldenEye-Recomp
cmake --build --preset linux-amd64-relwithdebinfo --target ge
```
Expected: builds clean (relinks `librexruntimerd.so`), no new warnings from `pipeline_cache.cpp`.

- [ ] **Step 4: Runtime verification (clean shutdown still stores, atomically)**

```bash
cd /home/keith/Projects/GoldenEye-Recomp
stat -c '%y %s' ~/.local/share/ge/cache/shaders/shareable/*.vk.pcache
LD_LIBRARY_PATH=../GoldenEye-Recomp-rexglue/out/linux-amd64 \
  ./out/build/linux-amd64-relwithdebinfo/ge --game_data_root=$PWD/assets --log_level info
# reach the main menu (pipelines get created), then quit normally (close window)
stat -c '%y %s' ~/.local/share/ge/cache/shaders/shareable/*.vk.pcache
ls ~/.local/share/ge/cache/shaders/shareable/*.tmp 2>/dev/null || echo "no tmp remnants"
grep "Stored persistent VkPipelineCache" out/build/linux-amd64-relwithdebinfo/logs/ge_*.log | tail -1
```
Expected: blob mtime updated after the run, `no tmp remnants`, and the `Stored persistent VkPipelineCache to ... (N bytes)` log line present. (If the log lands elsewhere, add `--log_file=/tmp/claude-1000/.../ge-task1.log` and grep that.)

- [ ] **Step 5: Commit**

```bash
cd /home/keith/Projects/GoldenEye-Recomp-rexglue
git add src/graphics/vulkan/pipeline_cache.cpp
git commit -m "fix(vulkan): make persistent pipeline-cache store atomic (tmp+rename) and log it"
```

---

### Task 2: Dirty flag + quiescence-debounced store on the storage-write thread

**Files:**
- Modify: `/home/keith/Projects/GoldenEye-Recomp-rexglue/include/rex/graphics/vulkan/pipeline_cache.h:392-393` (member block) 
- Modify: `/home/keith/Projects/GoldenEye-Recomp-rexglue/src/graphics/vulkan/pipeline_cache.cpp` — includes (line ~14), `EnsurePipelineCreated` (~line 3697, after successful `vkCreateGraphicsPipelines`), `StorageWriteThread` idle wait (~line 3878, the `if (!shader && !write_pipeline)` block)

**Interfaces:**
- Consumes: `StorePipelineDiskCache()` from Task 1 (void, no args, safe to call from the storage-write thread while creation threads are live — `vkGetPipelineCacheData` is internally synchronized; `ShutdownShaderStorage` joins this thread before clearing `pipeline_disk_cache_path_`).
- Produces: new private members `pipeline_disk_cache_dirty_` (`std::atomic<bool>`) and `pipeline_disk_cache_last_create_ms_` (`std::atomic<int64_t>`); file-local `kPipelineDiskCacheQuiescentMs` and `PipelineDiskCacheNowMs()` in the cpp. No external interface changes.

- [ ] **Step 1: Add `<chrono>` to the cpp std includes**

In `src/graphics/vulkan/pipeline_cache.cpp`, in the std includes block (lines 12–22), after `#include <atomic>` add:

```cpp
#include <chrono>
```

- [ ] **Step 2: Add the members to the header**

In `include/rex/graphics/vulkan/pipeline_cache.h`, directly below
`std::filesystem::path pipeline_disk_cache_path_;` (line 393), add:

```cpp
  // Set when a pipeline has been created against pipeline_disk_cache_ since
  // the last store. The storage write thread persists the cache once no new
  // creation has been stamped for a quiescence window — Android never gets a
  // clean shutdown, so waiting for ShutdownShaderStorage() would lose it.
  std::atomic<bool> pipeline_disk_cache_dirty_{false};
  std::atomic<int64_t> pipeline_disk_cache_last_create_ms_{0};
```

- [ ] **Step 3: Add the file-local constant and clock helper in the cpp**

In `src/graphics/vulkan/pipeline_cache.cpp`, immediately above
`void VulkanPipelineCache::SeedPipelineDiskCacheFromDisk() {` (line 356), add:

```cpp
namespace {

// Persist the disk pipeline cache after this long with no new pipeline
// creations (see pipeline_disk_cache_dirty_ in the header).
constexpr int64_t kPipelineDiskCacheQuiescentMs = 10000;

int64_t PipelineDiskCacheNowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

}  // namespace
```

- [ ] **Step 4: Stamp dirty on successful pipeline creation**

In `EnsurePipelineCreated`, right before the final `return true;` (line ~3715, after the placeholder/exchange block), add:

```cpp
  if (!pipeline_disk_cache_path_.empty()) {
    pipeline_disk_cache_last_create_ms_.store(PipelineDiskCacheNowMs(),
                                              std::memory_order_relaxed);
    pipeline_disk_cache_dirty_.store(true, std::memory_order_release);
  }
```

(This is the single funnel: both the synchronous CP path and the creation
worker threads land here, and `vkCreateGraphicsPipelines` at line 3681 always
uses `pipeline_disk_cache_`.)

- [ ] **Step 5: Replace the idle wait in `StorageWriteThread`**

In `StorageWriteThread`, replace:

```cpp
      if (!shader && !write_pipeline) {
        storage_write_request_cond_.wait(lock);
        continue;
      }
```

with:

```cpp
      if (!shader && !write_pipeline) {
        if (!pipeline_disk_cache_dirty_.load(std::memory_order_acquire)) {
          storage_write_request_cond_.wait(lock);
          continue;
        }
        int64_t idle_ms = PipelineDiskCacheNowMs() -
                          pipeline_disk_cache_last_create_ms_.load(std::memory_order_relaxed);
        if (idle_ms < kPipelineDiskCacheQuiescentMs) {
          storage_write_request_cond_.wait_for(
              lock, std::chrono::milliseconds(kPipelineDiskCacheQuiescentMs - idle_ms));
          continue;
        }
        // Quiesced. Clear the flag before storing so a creation racing with
        // the store re-arms it, then store outside the request lock.
        pipeline_disk_cache_dirty_.store(false, std::memory_order_relaxed);
        lock.unlock();
        StorePipelineDiskCache();
        continue;
      }
```

Notes for the implementer:
- Both `continue`s re-enter the loop top, which re-locks and re-checks `storage_write_thread_shutdown_` — shutdown wakes the `wait_for` via `notify_all` and exits promptly, same as today.
- `ShutdownShaderStorage` joins this thread **before** it closes files / clears `pipeline_disk_cache_path_` (cpp lines 862–869 vs 890), so the store can never race storage teardown.
- The clean-shutdown store at line 890 stays as-is.

- [ ] **Step 6: Build**

```bash
cd /home/keith/Projects/GoldenEye-Recomp
cmake --build --preset linux-amd64-relwithdebinfo --target ge
```
Expected: clean build.

- [ ] **Step 7: Runtime verification — kill-without-shutdown now persists the cache**

```bash
cd /home/keith/Projects/GoldenEye-Recomp
# Start from a cold cache so the debounced store is the only possible writer:
rm -f ~/.local/share/ge/cache/shaders/shareable/*.vk.pcache
LD_LIBRARY_PATH=../GoldenEye-Recomp-rexglue/out/linux-amd64 \
  ./out/build/linux-amd64-relwithdebinfo/ge --game_data_root=$PWD/assets --log_level info &
GE_PID=$!
# Reach the main menu, then leave it idle ~15s (menu pipelines quiesce), then:
kill -9 $GE_PID
ls -la ~/.local/share/ge/cache/shaders/shareable/
```
Expected: a `584108A9.*.vk.pcache` file exists (written by the debounce, since SIGKILL skipped clean shutdown), no `.tmp` file. Then relaunch and confirm the seed:

```bash
LD_LIBRARY_PATH=../GoldenEye-Recomp-rexglue/out/linux-amd64 \
  ./out/build/linux-amd64-relwithdebinfo/ge --game_data_root=$PWD/assets --log_level info \
  2>&1 | grep -m1 "Seeded persistent VkPipelineCache"
```
Expected: `VulkanPipelineCache: Seeded persistent VkPipelineCache from ... (N bytes)` — then quit the game normally.

- [ ] **Step 8: Commit**

```bash
cd /home/keith/Projects/GoldenEye-Recomp-rexglue
git add include/rex/graphics/vulkan/pipeline_cache.h src/graphics/vulkan/pipeline_cache.cpp
git commit -m "feat(vulkan): persist the pipeline disk cache on a 10s creation-quiescence debounce

Android never runs clean shutdown, so the .vk.pcache blob was never written
on device and every session recompiled all pipelines cold."
```

---

### Task 3: Thor on-device verification

**Files:** none (verification only).

**Interfaces:**
- Consumes: the `Stored persistent VkPipelineCache` / `Seeded persistent VkPipelineCache` log lines from Tasks 1–2; ge.log at `/sdcard/Android/data/com.sunjaycy.goldeneye/files/ge.log` (truncates per run).

- [ ] **Step 1: Build + install the debug APK**

```bash
cd /home/keith/Projects/GoldenEye-Recomp/android
./gradlew :app:installDebug -PrexSdkDir=/home/keith/Projects/GoldenEye-Recomp-rexglue
```
Expected: BUILD SUCCESSFUL, installs on the Thor (`adb devices` shows `192.168.1.182:41285`; if multiple devices, add `-s 192.168.1.182:41285` to every adb call below).

- [ ] **Step 2: Cold-cache kill test**

```bash
ADB="adb -s 192.168.1.182:41285"
$ADB shell rm -f "/sdcard/Android/data/com.sunjaycy.goldeneye/files/cache/shaders/shareable/*.vk.pcache"
$ADB shell monkey -p com.sunjaycy.goldeneye 1
```
Play/observe ~60s past the menu (get in-game so real pipelines compile), leave input idle ~15s, then kill WITHOUT clean shutdown:

```bash
$ADB shell am force-stop com.sunjaycy.goldeneye
$ADB shell ls -la /sdcard/Android/data/com.sunjaycy.goldeneye/files/cache/shaders/shareable/
```
Expected: a `584108A9.*.vk.pcache` file with a fresh timestamp, no `.tmp`.

- [ ] **Step 3: Relaunch seeds the cache**

```bash
$ADB shell monkey -p com.sunjaycy.goldeneye 1
sleep 30
$ADB shell grep -a "VkPipelineCache" /sdcard/Android/data/com.sunjaycy.goldeneye/files/ge.log
```
Expected: `Seeded persistent VkPipelineCache from ... (N bytes)` in this run's ge.log, and the game boots to a live render as usual (boot loader reaches `rendered#`).

- [ ] **Step 4: Record results**

Note the blob size and, subjectively, whether menu/level-load warm-up feels shorter on the second launch. (Objective A/B of first-shot hitches belongs to deep-dive item 2's instrumentation.)

---

### Task 4: Finish the branch

- [ ] **Step 1: Final review of the SDK diff**

```bash
cd /home/keith/Projects/GoldenEye-Recomp-rexglue
git diff main...feat/pipeline-cache-persistence
```

- [ ] **Step 2: Merge to SDK `main`** (repo convention: SDK work lands on `main`; confirm with the user if anything in Task 3 was inconclusive)

```bash
git checkout main
git merge --no-ff feat/pipeline-cache-persistence -m "merge: pipeline disk-cache persistence (Android kill-safe)"
```

- [ ] **Step 3: Commit the spec + plan docs in the game repo**

```bash
cd /home/keith/Projects/GoldenEye-Recomp
git add docs/superpowers/specs/2026-07-10-android-pipeline-cache-persistence-design.md \
        docs/superpowers/plans/2026-07-10-android-pipeline-cache-persistence.md
git commit -m "docs: pipeline-cache persistence spec + plan (perf deep-dive item 1)"
```
