# Runtime Missing-File Error Dialog — Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Add a runtime hook that shows a user-friendly error dialog whenever the running XEX tries to open a host file that does not exist.

**Architecture:** Add a callback to the SDK's `VirtualFileSystem::OpenFile`; register the callback from `GeApp::OnPreLaunchModule`; filter out benign optional probes in a new `ge_missing_file.cpp`; show the error from the UI thread and quit. A cvar `ge_fatal_on_missing_file` controls whether the dialog is fatal.

**Tech Stack:** C++23, ReXGlue SDK, CMake, `rex::filesystem::VirtualFileSystem`, `rex::ShowSimpleMessageBox`, `rex::cvar`.

---

## Repo layout notes

- Game repo: `/home/keith/Projects/GoldenEye-Recomp`
- SDK repo: `/home/keith/Projects/GoldenEye-Recomp-rexglue` (sibling checkout, referenced by `REXSDK_DIR`)
- The `ge` target links `librexruntimerd.so` from the SDK automatically when rebuilt.

---

### Task 1: SDK — extend `VirtualFileSystem` with a missing-file callback

**Files:**
- Modify: `../GoldenEye-Recomp-rexglue/include/rex/filesystem/vfs.h`

Add `MissingFileInfo` struct, callback type, setter, and member. Exact code in design doc.

**Commit:** `git commit -m "vfs: add missing-file callback hook"`

---

### Task 2: SDK — invoke the callback from `OpenFile`

**Files:**
- Modify: `../GoldenEye-Recomp-rexglue/src/filesystem/virtual_file_system.cpp`

In the `kOpen`/`kOverwrite` + `!entry` branch, before returning `X_STATUS_NO_SUCH_FILE`, compute the host path and invoke `missing_file_callback_` if set.

**Commit:** `git commit -m "vfs: invoke missing-file callback on Open/Overwrite failure"`

---

### Task 3: Game — create `ge_missing_file` handler

**Files:**
- Create: `src/ge_missing_file.h`
- Create: `src/ge_missing_file.cpp`

Implement `OnMissingFile` that filters by disposition, path prefix, and optional probes; logs; and (if `ge_fatal_on_missing_file` is true) posts a UI-thread message box + quit. Expose `SetMissingFileAppContext`.

**Commit:** `git commit -m "feat: add runtime missing-file handler"`

---

### Task 4: Game — register the callback in `GeApp`

**Files:**
- Modify: `src/ge_app.h`

Include `ge_missing_file.h`, forward-declare `SetMissingFileAppContext`, and override `OnPreLaunchModule` to set the app context and register the VFS callback.

**Commit:** `git commit -m "feat: register missing-file callback in GeApp"`

---

### Task 5: Game — wire into build

**Files:**
- Modify: `CMakeLists.txt`

Add `src/ge_missing_file.cpp` to `GE_SOURCES`.

**Commit:** `git commit -m "build: add ge_missing_file.cpp to GE_SOURCES"`

---

### Task 6: Build and test

**Commands:**

```bash
cd /home/keith/Projects/GoldenEye-Recomp
cmake --build --preset linux-amd64-relwithdebinfo --target ge
```

Expected: build succeeds.

**Test 1 — normal boot:**
```bash
LD_LIBRARY_PATH=../GoldenEye-Recomp-rexglue/out/linux-amd64 \
  ./out/build/linux-amd64-relwithdebinfo/GoldenEye \
  --game_data_root=$PWD/assets --log_level debug
```
Expected: no dialog.

**Test 2 — missing texture:**
```bash
mv assets/files/texture/bg/where/default.rba assets/files/texture/bg/where/default.rba.bak
LD_LIBRARY_PATH=../GoldenEye-Recomp-rexglue/out/linux-amd64 \
  ./out/build/linux-amd64-relwithdebinfo/GoldenEye \
  --game_data_root=$PWD/assets --log_level debug 2>&1 | tee /tmp/ge_missing.log
mv assets/files/texture/bg/where/default.rba.bak assets/files/texture/bg/where/default.rba
```
Expected: message box appears; log contains `Missing game file:`.

**Test 3 — cvar disabled:**
```bash
mv assets/files/texture/bg/where/default.rba assets/files/texture/bg/where/default.rba.bak
LD_LIBRARY_PATH=../GoldenEye-Recomp-rexglue/out/linux-amd64 \
  ./out/build/linux-amd64-relwithdebinfo/GoldenEye \
  --game_data_root=$PWD/assets --ge_fatal_on_missing_file=false \
  --log_level debug 2>&1 | tee /tmp/ge_missing_nofatal.log
mv assets/files/texture/bg/where/default.rba.bak assets/files/texture/bg/where/default.rba
```
Expected: no dialog; log contains warning.

**Test 4 — missing default.xex still handled by SDK:**
```bash
mv assets/default.xex assets/default.xex.bak
LD_LIBRARY_PATH=../GoldenEye-Recomp-rexglue/out/linux-amd64 \
  ./out/build/linux-amd64-relwithdebinfo/GoldenEye \
  --game_data_root=$PWD/assets 2>&1 | tee /tmp/ge_no_xex.log
mv assets/default.xex.bak assets/default.xex
```
Expected: existing SDK dialog appears.

**Commit any fixes.**

---

## Execution handoff

Plan complete and saved to `docs/plans/2026-07-09-missing-file-error-plan.md`. Two execution options:

1. **Subagent-Driven (this session)** — dispatch a fresh subagent per task, review between tasks, fast iteration.
2. **Parallel Session (separate)** — open a new session with `superpowers:executing-plans`, batch execution with checkpoints.

Which approach would you like?
