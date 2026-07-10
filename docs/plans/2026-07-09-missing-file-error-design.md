# Runtime Missing-File Error Dialog

## Goal
Catch any file the running GoldenEye XEX tries to open that does not exist on the host, and present a clear error to the user instead of letting the game silently fail or crash later.

## Background
The ReXGlue SDK already verifies that `assets/default.xex` exists during `ConstructRuntime`, but it does not check the many other game files the XEX loads at runtime (textures in `files/texture/…`, audio banks, localization strings, etc.). When one of those files is absent, the game usually just hangs or boots into a broken state. This design adds a runtime hook so the user gets an immediate, actionable message.

## Architecture
Add a callback hook in the SDK’s `VirtualFileSystem::OpenFile` that fires when an `kOpen`/`kOverwrite` request resolves to `X_STATUS_NO_SUCH_FILE`. The game registers a callback in `GeApp::OnPreLaunchModule`. The callback filters out benign optional probes, then posts a UI-thread message box and quits.

## SDK changes (`GoldenEye-Recomp-rexglue`)

### `include/rex/filesystem/vfs.h`
- Add a `MissingFileInfo` struct carrying:
  - `std::string_view guest_path` (e.g. `game:\files\texture\foo.rba`)
  - `std::filesystem::path host_path` (resolved host path for the message)
  - `FileDisposition disposition`
- Add `using MissingFileCallback = std::function<void(const MissingFileInfo&)>;`
- Add `void SetMissingFileCallback(MissingFileCallback cb);`

### `src/filesystem/virtual_file_system.cpp`
- In `OpenFile`, in the `kOpen`/`kOverwrite` + `!entry` branch (where it returns `X_STATUS_NO_SUCH_FILE`), before returning:
  - Resolve the matching `HostPathDevice` and compute `host_path`.
  - If a callback is set, invoke it.
- Keep the callback invocation outside the VFS critical section (the current code already is).

## Game changes (`GoldenEye-Recomp`)

### New files: `src/ge_missing_file.h` / `src/ge_missing_file.cpp`
- `void OnMissingFile(const rex::filesystem::MissingFileInfo& info)` — thread-safe handler.
- Filters:
  - Only paths under `game:\` or `d:\`.
  - Only `kOpen` / `kOverwrite` dispositions.
  - Skip known optional probes (`loc\…`, cache paths, config probes).
  - Deduplicate so the same missing path is reported only once.
- On the first non-filtered miss, use `app_context().CallInUIThread(...)` to show `rex::ShowSimpleMessageBox(Error, …)` and then `QuitFromUIThread()`.
- Also log the miss at error level so it appears in `ge.log` / logcat.

### `src/ge_app.h`
- Override `OnPreLaunchModule()`:
  ```cpp
  void OnPreLaunchModule() override {
    runtime()->file_system()->SetMissingFileCallback(
        [](const auto& info) { ge::OnMissingFile(info); });
  }
  ```

### `CMakeLists.txt`
- Add `src/ge_missing_file.cpp` to `GE_SOURCES`.

### Cvar
- Add `ge_fatal_on_missing_file` (bool, default `true`).
  - When `true`, the first non-filtered missing file shows the error dialog and quits.
  - When `false`, missing files are only logged; no dialog.
  - Useful for power users who want to test incomplete asset sets.

## UX / error message
Example message box text:

> **Missing game file**
>
> The game tried to open a file that does not exist:
> `assets/files/texture/bg/where/default.rba`
> (guest path: `game:\files\texture\bg\where\default.rba`)
>
> Make sure all GoldenEye 007 game files are placed in the `assets/` folder.

On Android, `ShowSimpleMessageBox` may be limited; the same text is written to the log and the app exits.

## Testing plan
1. Delete a non-critical `.rba` texture and launch — expect the message box.
2. Delete `default.xex` — the existing SDK pre-check should still fire first.
3. Launch normally and verify no message box appears (optional loc probes are filtered).
4. Verify the missing path is written to `ge.log`.
5. Set `--ge_fatal_on_missing_file=false`, delete a file, and verify the game logs the miss but does not quit.

## Decisions
- 2026-07-09: Use an SDK VFS callback rather than a guest hook or manifest pre-check. Chosen because it catches real runtime misses, shows the exact host path, and requires only a small SDK change plus one game hook.
- 2026-07-09: Add `ge_fatal_on_missing_file` cvar (default `true`) so the behavior can be disabled for testing.
- 2026-07-09: On Android, fall back to logging + quit because native message boxes are unreliable in NativeActivity.
