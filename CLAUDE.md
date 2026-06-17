# GoldenEye-Recomp — notes for Claude

Xbox 360 GoldenEye recompilation. The game target `ge` links the **ReXGlue SDK** at the
sibling checkout `../GoldenEye-Recomp-rexglue` (`REXSDK_DIR`), built as a subproject — so
editing SDK source and rebuilding `ge` recompiles/relinks `librexruntimerd.so` automatically.

## Active handoff / open issue
- **arm64 (Android) first-frame freeze — FIXED (2026-06-17), verified on the Ayn Thor.**
  Root cause: the arm64 GPU-completion path blocked on `rex_ge_cp_wait_progress` (added to
  stop yield-spinning guest threads starving the single CP worker on few-core handhelds), but
  the ~2ms blocking re-check let `ge_dbg_now`'s 80ms skip-bit fallback latch → render skipped
  forever — the *same* freeze the desktop bisect (`9dd0258`) pinned on the blocking wait. Fix:
  boost the rexglue CP worker to kAboveNormal at creation (`command_processor.cpp`,
  `creation_flags=0x20`) so it can't be starved, then unify both arches on the proven-good
  `std::this_thread::yield()` (removed the arm64 blocking-wait branch in `ge_dbg_now`). Verified
  9/9 cold boots reach a live render on the first attempt (vs ~50% freeze baseline), plus a 30s
  soak at ~60fps with zero `GEWATCHDOG STALL`. The boot loader now gates on a new freeze-proof
  `GEGPU rendered#N` heartbeat (emitted only when a frame is really drawn) instead of `present#`
  (which advances even on a frozen boot). See
  **[`docs/HANDOFF-arm64-first-frame-freeze.md`](docs/HANDOFF-arm64-first-frame-freeze.md)** for the
  full investigation, the corrected diagnosis (the handoff's "completion counter stuck at 0" was a
  misread of the WAIT_REG_MEM semaphore), and the build/debug recipe.

## Build / run quick reference
- **Linux:** `cmake --build --preset linux-amd64-relwithdebinfo --target ge`; run with
  `LD_LIBRARY_PATH=../GoldenEye-Recomp-rexglue/out/linux-amd64` and `--game_data_root=$PWD/assets`
  (cvars are CLI flags, e.g. `--log_level debug`, `--ge_freeze_diag`).
- **Android (Ayn Thor handheld, arm64):** `cd android && ./gradlew :app:installDebug -PrexSdkDir=/home/keith/Projects/GoldenEye-Recomp-rexglue`
  (the `-PrexSdkDir` absolute path is required; `gradle.properties` resolves it wrong). Install
  downgrade → `adb install -r -d`. No config file is read on Android — set cvars via
  `SetFlagByName` in `GeApp::OnConfigurePaths` (`src/ge_app.h`). Logs: `/sdcard/Android/data/com.sunjaycy.goldeneye/files/{ge.log,stderr.txt}`.

See `docs/HANDOFF-arm64-first-frame-freeze.md` for the complete recipe, gotchas, and the
audio / boot-loader fixes made alongside this.
