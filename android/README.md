# GoldenEye 007 — Android (arm64) build

NativeActivity shell that packages the recompiled game (`libge.so`) + the ReXGlue
runtime (`librexruntime.so`) into an APK. Controller-driven; no touch controls.

> **Status:** the ReXGlue SDK builds end-to-end for `arm64-v8a`, and the Android
> window/Vulkan-surface/input-loop backends are implemented. A *runnable* APK
> additionally needs (a) the recompiled game code from your own XEX and (b) the
> remaining runtime work tracked in the SDD plan (real fibers, content-URI fd,
> gamepad input driver). See the project plan for the full status.

## Prerequisites

- Android SDK + **NDK 27.1.12297006** (Vulkan-capable).
- A local checkout of the **ReXGlue SDK**: `SunJaycy/GoldenEye-Recomp-rexglue`
  (point `rexSdkDir` at it — default `../../GoldenEye-Recomp-rexglue`).
- The `rexglue` CLI and **your own GoldenEye 007 XBLA XEX** (this repo ships none).

## Build steps

```sh
# 1. From the repo root: generate the recompiled game code from YOUR copy.
rexglue codegen --max_jump_table_entries 2048 ge_config.toml   # creates generated/

# 2. Build the APK (CMake is driven by Gradle's externalNativeBuild).
cd android
./gradlew assembleRelease -PrexSdkDir=/path/to/GoldenEye-Recomp-rexglue

# APK: android/app/build/outputs/apk/release/
```

## How it wires together

- `AndroidManifest.xml` declares `android.app.NativeActivity` with
  `android.app.lib_name=ge` → loads `libge.so`.
- The SDK's `rexglue_setup_target` links `android_native_app_glue` (exports
  `ANativeActivity_onCreate`) and `windowed_app_main_android.cpp` (`android_main`).
- `android_main` creates an `AndroidWindowedAppContext`, looks up the registered
  app, and runs the ALooper loop. `APP_CMD_INIT_WINDOW` hands the `ANativeWindow`
  to `AndroidWindow`, which builds a `VK_KHR_android_surface` for the existing
  Vulkan renderer.

## Notes / current limitations

- `arm64-v8a` only (the guest reserves a 4 GiB address space).
- Render backend is Vulkan; SDL3 (GLES/AAudio) is bundled for audio/input.
- Game assets/XEX delivery (APK assets vs app-scoped storage) is not yet wired —
  see `OpenAndroidContentFileDescriptor` (Phase 2 TODO) and the SDK filesystem.
