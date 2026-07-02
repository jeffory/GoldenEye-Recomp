# Dual-screen weapon menu — integration, config, fallback, validation

This documents the final integration of the second-screen weapon-selection menu
(AYN Thor) on the `feat/ds-support` branch, how to turn it on/off, how it behaves
on single-screen devices, and how to validate it.

## What it is

On a device with a secondary display (the AYN Thor's 3.92" bottom touch panel),
the game renders an ImGui weapon-selection menu on that panel. It shows the
player's carried weapons + ammo, highlights the equipped weapon, and lets the
player tap a weapon to switch to it. The main game on the primary screen is
unchanged.

## The pieces (and where each came from)

| Layer | Repo / files | Sub-task |
|---|---|---|
| Secondary render surface (ImGui-only Presenter/Surface/swapchain on an external native window) | `GoldenEye-Recomp-rexglue`: `rex/ui/external_window.*`, `rex/ui/secondary_ui_surface.*` | HOM-159 |
| Guest game-state bridge (`GetWeaponSnapshot` / `RequestEquipWeapon`) | `GoldenEye-Recomp`: `src/ge_gamestate.*` | HOM-160 |
| Android secondary-display binding (DisplayManager → Presentation → Surface → ANativeWindow) | `GoldenEye-Recomp`: `WeaponMenuPresentation.java`, `GoldenEyeActivity.java`, `src/ge_android_ds.cpp` | A2 (HOM-161) |
| Weapon-menu UI (the ImGui dialog) | `GoldenEye-Recomp`: `src/ge_weaponmenu.*` | C (HOM-162) |
| Controller / wiring / config / fallback (this task) | `GoldenEye-Recomp`: `src/ge_dualscreen.*`, `ge_app.h`, `ge_hooks.cpp`, `ge_menu.cpp` | HOM-163 |

> A2 and C had not been started when this integration ran, so they were
> implemented here as part of the integration pass.

## How it is driven (threading)

The secondary surface must be created, painted and destroyed **on the UI thread**
(`SecondaryUiSurface` asserts this). The guest "present" hook (`ge_diag_vdswap` in
`ge_hooks.cpp`) runs on the **game thread**. So every presented frame the game
thread calls `ge::DualScreen::OnGuestPresent()`, which posts **one** UI-thread
tick via `WindowedAppContext::CallInUIThreadDeferred` (guarded by an atomic so at
most one paint is ever outstanding, regardless of frame rate). All surface
creation, painting, teardown and touch injection happen inside that tick.

We deliberately do **not** self-re-arm a deferred callback: the SDK drains its
deferred queue in a `while (!empty())` loop, so a callback that re-posts itself
would spin forever within a single drain. Posting a fresh one-shot per present is
the correct, bounded mechanism and needs **no rexglue SDK changes**.

## Config toggle

`ge_ds_weapon_menu` (bool cvar, `Video` category, default **true**), defined in
`ge_dualscreen.cpp`. UI: **pause menu → VIDEO → SECOND SCREEN → "Weapon menu on
second screen"**. Persisted to `ge.toml` like every other pause-menu setting.
Turning it off destroys the secondary surface on the next tick and stops all
secondary work; turning it back on re-creates it when a secondary display is
present.

## Single-screen fallback policy

The feature is **inactive at zero cost** on any device without a secondary
display (desktop, ordinary phones, the Thor with the bottom panel off):

- **Java**: `pickSecondaryDisplay()` returns null when there is no non-default /
  presentation display, so no `Presentation` is created and native is never handed
  a surface. A `DisplayManager.DisplayListener` plus `onResume`/`onPause` keep this
  correct across hotplug, dock/undock, and backgrounding.
- **Native**: `DualScreen` does nothing until `ProvideSecondaryWindow()` is
  called. `OnGuestPresent()` still runs each frame but is a couple of atomic
  loads and an early return — no allocation, no paint, no guest-memory access
  beyond the existing bridge pump.
- **No behavioral change to the main game**: the primary surface, its presenter
  and its ImGui context are never touched by any of this.

### Decision: no on-primary-screen overlay fallback (by default)

The task asked whether to offer the weapon menu as an on-primary-screen overlay
when there is no second screen. **Decision: no, not by default.** Rationale:

- The acceptance criterion is "cleanly inactive with no perf cost and **no
  behavioral change to the main game**" on single-screen devices. An overlay on
  the primary screen is, by definition, a behavioral change to the main game.
- GoldenEye already has an in-game weapon cycle; a permanent on-screen weapon
  grid would obscure gameplay and duplicate existing input.

If an opt-in primary overlay is wanted later it is a small, isolated addition (a
second cvar that, when set and no secondary display exists, attaches a
`WeaponMenuDialog` to the **primary** drawer). It is intentionally left out so the
default single-screen experience is pixel-identical to today.

## Build

Nothing new is required beyond the existing flow:

- **Desktop / Android**: `rexglue codegen` against your own GoldenEye 007 XEX
  (produces `generated/`), then build as before (`cmake --build` for desktop;
  `android/gradlew assembleDebug|assembleRelease` for the arm64 APK). The new
  sources (`ge_dualscreen.cpp`, `ge_weaponmenu.cpp`, and Android-only
  `ge_android_ds.cpp`) are wired into `CMakeLists.txt`.
- **CI**: `ge_dualscreen.cpp` and `ge_weaponmenu.cpp` are added to the
  compile-verify list (SDK-only, no generated headers); `ge_android_ds.cpp` is
  syntax-checked in the Android (NDK) job. As before, the final `ge` binary is not
  built in CI (it needs the proprietary XEX).

## Validation

### Status of empirical validation

- **Builds**: the new cross-platform sources compile against the SDK headers
  (same compile-verify path CI uses); see the PR checks. A full link of `ge`
  requires `rexglue codegen` + a GoldenEye XEX, which is not available in this
  environment, so a full binary build was not run here.
- **AYN Thor hardware**: not available in this environment, so the on-device
  end-to-end demo (game on top, live menu on bottom, tap-to-switch) has **not**
  been run. The code follows the source-verified Android pattern from the spike
  (HOM-158): the Thor exposes the bottom panel as a standard presentation
  `Display`, driven via `Presentation` + a `SurfaceView`.
- **Live weapon data**: the game-state bridge ships with its guest offset
  constants at 0 (`ge_gamestate.cpp`), which keeps it **inert** (`valid=false`).
  Until those offsets are filled in on-device (procedure in
  `docs/ds-game-state-bridge.md`), the menu renders its "Weapon data unavailable"
  state rather than wrong data. The surface, layout, touch path and equip plumbing
  are exercised independent of the offsets.

### On-device acceptance checklist (to run on a Thor or equivalent)

1. Fill in the bridge offsets (see `docs/ds-game-state-bridge.md`) and rebuild.
2. Launch the APK on the Thor. Confirm the main game runs on the top screen and
   the weapon menu appears on the bottom panel.
3. Tap a weapon on the bottom panel → the equipped weapon switches in-game and
   the highlight follows.
4. Toggle **pause menu → VIDEO → "Weapon menu on second screen"** off/on → the
   bottom-screen menu disappears/reappears with no effect on the top screen.
5. Display connect/disconnect: dock/undock or toggle the panel → menu tears down
   and re-establishes without a crash.
6. Pause/resume (home + return) and quit → no crash on teardown.

### Single-screen stand-in (no Thor)

- Desktop / ordinary phone: launch normally. The feature stays inactive (no second
  surface created); confirm the main game is unchanged. This exercises the
  fallback path.
- The rexglue `examples/secondary_ui` xcb demo (from HOM-159) renders a
  `SecondaryUiSurface` in a real second X11 window on Linux and is the standalone
  visual proof of the secondary-surface + ImGui path without hardware.
