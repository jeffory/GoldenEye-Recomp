# Android Vulkan Pipeline-Cache Persistence — Design

**Date:** 2026-07-10
**Repos touched:** SDK only (`../GoldenEye-Recomp-rexglue`, `src/graphics/vulkan/pipeline_cache.cpp/.h`)
**Status:** Approved

## Problem

The persistent native `VkPipelineCache` blob (`<cache>/shaders/shareable/<title>.<vendor>_<device>_<driver>.vk.pcache`,
~1–2MB) is only written by `StorePipelineDiskCache()`, whose sole steady-state call site is
`ShutdownShaderStorage()` — i.e. clean shutdown. Android kills the app instead of shutting it
down, so the blob never reaches disk on the Ayn Thor (verified: device has `584108A9.xsh` and
`584108A9.fbo.vk.xpso`, which are written incrementally, but no `.vk.pcache`; the Linux install
has both `.vk.pcache` blobs). Every Android session therefore compiles every Vulkan pipeline
cold, contributing first-encounter hitches (menus, level loads, first firefight) every launch
instead of only on the first-ever launch.

## Design

Dirty-flag + quiescence debounce inside `VulkanPipelineCache`, enabled on all platforms
(Linux gains crash-safety; behavior on clean shutdown is unchanged).

1. **Dirty tracking.** Wherever a pipeline is actually created against `pipeline_disk_cache_`
   (the synchronous `EnsurePipelineCreated` path and the creation-worker path), set an atomic
   `pipeline_disk_cache_dirty_` flag and stamp `pipeline_disk_cache_last_create_` with a
   monotonic timestamp.
2. **Debounced store.** The existing `storage_write_thread_` loop (already owns all storage
   file writes, already has a condvar wait) switches to a timed wait while the dirty flag is
   set: when dirty **and** no new pipeline creation has been stamped for **10 seconds**, call
   `StorePipelineDiskCache()` and clear the flag. New creations during the window push the
   deadline back. `vkGetPipelineCacheData` on a live `VkPipelineCache` is internally
   synchronized against concurrent `vkCreate*Pipelines`, so no new locking is needed around
   the Vulkan call itself; path/handle lifetime is protected by performing the store on the
   storage-write thread, which is joined before `ShutdownShaderStorage` clears
   `pipeline_disk_cache_path_`.
3. **Atomic write.** `StorePipelineDiskCache()` writes to `<path>.tmp` then renames over the
   final path (today it writes in place). An OOM-kill mid-write can no longer corrupt the seed
   blob; `SeedPipelineDiskCacheFromDisk()` is unchanged (a stale-but-valid blob is fine — the
   driver validates the header and falls back to empty).
4. **Clean shutdown unchanged.** `ShutdownShaderStorage()` keeps its store call as a final
   catch-up; it becomes a no-op-cost second write at worst.

No new cvar: the feature is already gated by `vulkan_persistent_pipeline_cache` (default true);
the debounce only runs when a disk-cache path is configured.

## Effect

After the first 10-second-quiet moment following any pipeline warm-up, the blob is on disk.
The next launch seeds it and `vkCreateGraphicsPipelines` becomes near-free for previously
seen state combos, even though the app was never cleanly shut down.

## Testing

- SDK builds pass for `linux-amd64-relwithdebinfo` and the Android arm64 gradle build.
- **Thor (primary):** install, launch, play ~1 min past the menu, swipe-kill the app (no clean
  shutdown), then verify `584108A9.*.vk.pcache` exists under
  `/sdcard/Android/data/com.sunjaycy.goldeneye/files/cache/shaders/shareable/` and that a
  relaunch logs the seed (grep ge.log for the pipeline-cache seed line) and boots normally.
- **Linux:** run, quit cleanly, verify the blob still updates (mtime) and no `.tmp` remnants;
  run + SIGKILL mid-session after >10s idle, verify blob present and next run seeds it.

## Risks

Low. One atomic flag, one timed wait on an existing worker, one rename. No change when
nothing is dirty. The 10s constant is a compile-time constant (not worth a cvar).
