# GoldenEye-Recomp — notes for Claude

Xbox 360 GoldenEye recompilation. The game target `ge` links the **ReXGlue SDK** at the
sibling checkout `../GoldenEye-Recomp-rexglue` (`REXSDK_DIR`), built as a subproject — so
editing SDK source and rebuilding `ge` recompiles/relinks `librexruntimerd.so` automatically.

## Active handoff / open issue
- **arm64 (Android) first-frame freeze** — the game boots, plays audio, renders the Twycross
  first frame, then freezes (GPU completion counter never advances; render skipped every frame).
  Root cause localized, fix open. Full investigation + evidence + build/debug recipe:
  **[`docs/HANDOFF-arm64-first-frame-freeze.md`](docs/HANDOFF-arm64-first-frame-freeze.md)** — read this first.

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
