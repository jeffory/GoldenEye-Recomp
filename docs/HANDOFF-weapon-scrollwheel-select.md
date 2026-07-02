# Handoff: desktop scrollwheel + number-key weapon selection

**Status (2026-07-03):** Task 1 (actuation) DONE, verified on desktop, and shipped
in prerelease `v1.3.0-android.3`. Tasks 2–4 not started. A few non-blocking
review cleanups on Task 1 are queued (below).

**Goal:** switch weapons on **desktop** with the mouse scrollwheel (next/prev) and
number keys 1–9 (jump to the Nth carried weapon), with a numbered on-screen
overlay. Desktop is the debug-first target; the end goal is weapon selection from
the Ayn Thor's **second screen** (which already lists weapons — and, as of Task 1,
its taps now actually switch weapons).

## Source-of-truth docs
- **Design spec:** `docs/superpowers/specs/2026-07-02-weapon-scrollwheel-select-design.md`
- **Implementation plan (revised for inject-Y):** `docs/superpowers/plans/2026-07-02-weapon-scrollwheel-select.md`
- **Deferred optimization (direct guest-function call):** `docs/HANDOFF-weapon-switch-direct-call.md`

## What shipped (Task 1 — actuation)
Selecting a weapon now switches it in-game. Mechanism: **inject the game's native
Y button** (weapon-switch) and cycle until the equipped weapon matches the target.
- API (`src/ge_gamestate.{h,cpp}`): `RequestEquipWeapon(id)` posts a target;
  `PeekEquipRequest()` reads it without clearing; `ClearEquipRequest()` clears.
- Driver (`src/ge_hooks.cpp`, in `ge_inject_keyboard`, placed **before** the
  `ge_keyboard_enable/ge_input_active` early-return so it runs regardless of the
  keyboard toggle): pulses `BTN_Y` into `GE_PAD0`, then **waits for the switch to
  land** (the snapshot's `equipped_id` changes) before pressing again. Tunables:
  `kMaxSteps=16`, `kStepTimeout=90` frames. Do NOT revert to a fixed-gap pulse —
  that queues presses and cycles straight past the target (the bug we hit).
- The old guest-memory `apply_equip` writer was **removed** (writing the
  equipped-id block `0x447f10b0` corrupts it; see the direct-call handoff).
- Commits: `bf3e03b` (actuation), `5b06348` (temp N key disabled for the release).

### Key decisions / dead-ends (so they aren't re-tried)
1. **Direct memory write of a weapon-id field — DISPROVEN.** `0x447f10b0` desyncs
   the id-agreement gate; `0x447f1104` sticks but does nothing. The game sets
   weapon state *from* its switch routine and does not poll a "desired weapon"
   field.
2. **Calling the guest switch function directly — DEFERRED** (would give instant
   jump-to-N). Trail captured in `docs/HANDOFF-weapon-switch-direct-call.md`
   (`sub_820A7508` is a per-hand applier; the real Y-driven entry is upstream and
   unidentified).
3. **Native-Y injection (chosen).** Guaranteed-correct, reuses the proven pad
   path; jump-to-N costs a few frames of cycling (imperceptible).

## Pending — Task 1 review cleanups (non-blocking, do before/with Task 2)
From the task review (all in the Task 1 code):
1. **Stale header docs** in `src/ge_gamestate.h` — the `RequestEquipWeapon` and
   `OnFrame` comments and the top-of-file contract block still describe the
   removed `apply_equip` memory-write path and don't list `PeekEquipRequest` /
   `ClearEquipRequest`. (The `.cpp` header block was already updated.)
2. **No held/valid-id guard** in the driver — it will cycle toward any posted
   `target` (up to `kMaxSteps`). Current callers only post held ids, but add a
   guard: if `target` isn't in `snap.held_mask` (or is out of range), clear and
   abort. The stale header even claims unheld ids are ignored.
3. **Minor:** `ClearEquipRequest()` is an unconditional store, not a CAS against
   the peeked value — a `RequestEquipWeapon` landing between peek and clear on the
   same frame is dropped (single-thread, unlikely). And the driver's `steps`/`wait`
   counters aren't reset when a new target is posted mid-cycle.

## Remaining tasks (see the plan for full step-by-step)
- **Task 2 — desktop input driver** (`src/ge_hooks.cpp`): remove the temp N block
  (currently `#if 0`); poll `"WheelUp"`/`"WheelDown"` (SDK synthetic keys, via
  `ge_key_down`) for next/prev and digit keys 1–9 for jump-to-Nth-held, all
  edge-triggered, computing a target and calling `RequestEquipWeapon`. New cvars:
  `ge_weapon_select_enable` (default **on**), `ge_key_wpn_next` (`"WheelUp"`),
  `ge_key_wpn_prev` (`"WheelDown"`). Numbers map to held-list position.
- **Task 3 — numbered overlay** (`FpsOverlay` pattern): a bottom-left ImGui
  overlay on the main window listing carried weapons with the equipped one
  highlighted; cvar `ge_weapon_overlay` (default **off**). Factor `WeaponLabel`
  into a shared unit (`src/ge_weapons.{h,cpp}`) used by both the overlay and the
  existing `WeaponMenuDialog`. Register new `.cpp`s in `CMakeLists.txt`.
- **Task 4 — pause-menu toggle** for `ge_weapon_overlay` in `src/ge_menu.cpp`
  (hand-authored menu; use the `GetCvarB`/`SetCvarB` pattern next to the existing
  second-screen weapon-menu checkbox).

Each task ends with a manual on-desktop verification (build → run → observe);
there is no unit-test harness for guest-memory / ImGui behavior here.

## Build / run gotchas
- Build: `cmake --build --preset linux-amd64-relwithdebinfo --target ge`.
- **Run binary is `out/build/linux-amd64-relwithdebinfo/GoldenEye`, NOT `ge`**
  (CMake `OUTPUT_NAME "GoldenEye"`). `CLAUDE.md`'s run recipe says `ge` and is
  wrong; this also bit `cut-release.sh` (fixed, `57900c1`).
- Run: `LD_LIBRARY_PATH=../GoldenEye-Recomp-rexglue/out/linux-amd64 ./out/build/linux-amd64-relwithdebinfo/GoldenEye --game_data_root=$PWD/assets`
  (append cvars like `--log_level debug`, `--ge_gamestate_diag=true`).

## Discovery tooling (kept, desktop-only, behind `ge_gamestate_diag`)
The `memscan` scanner in `ge_gamestate.cpp` gained a live `write <ga> <16|32> <val>`
command (drive via `/tmp/ge_scan.cmd`, output `/tmp/ge_scan.out`). Useful for the
deferred direct-call RE.

## Resuming the SDD flow
This was executed via `superpowers:subagent-driven-development`. The revised plan
is the task list; the progress ledger lived at `.superpowers/sdd/progress.md`
(git-ignored scratch — may be gone in a fresh checkout; this handoff has the
essentials). Resume at the Task 1 cleanups, then Task 2.
