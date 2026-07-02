# Desktop weapon selection — scrollwheel + number keys

**Date:** 2026-07-02
**Status:** Design approved, ready for implementation plan
**Builds on:** the dual-screen weapon menu work (`WeaponSnapshot`, `WeaponLabel`,
`RequestEquipWeapon`, the `GE_PAD0` input-injection path).

## Goal

Let the player switch weapons on **desktop** with classic PC-FPS controls:

- **Mouse scrollwheel** cycles to the next / previous carried weapon.
- **Number keys 1–9** jump directly to the Nth carried weapon.
- A **numbered on-screen overlay** shows the carried weapons and highlights the
  equipped one.

Desktop is the deliberate first target because it is far easier to debug than
the Ayn Thor. The end goal is weapon selection driven from the Thor's second
screen (which already *displays* weapons via the dual-screen menu but cannot yet
*switch* them); wiring the actuation here makes that second-screen path start
working too, but Thor-specific input/UI is out of scope for this spec.

## Current state (what already exists)

- **Read path — works.** `gamestate::GetWeaponSnapshot()` publishes a thread-safe
  `WeaponSnapshot` each frame: `equipped_id`, dense `held_ids[0..held_count)`,
  `held_mask`, and `ammo[]`. Confirmed live via the fixed guest address
  `0x447f10b0` (equipped-id) plus the carried-weapons linked-list walk.
- **Equip request plumbing — exists but INERT.** `RequestEquipWeapon(id)` posts a
  lock-free request; `apply_equip()` consumes it on the game thread and is
  designed to write the guest "switch to this weapon" field. It currently
  **drops every request** because `kEquipRequestOff == 0` and
  `kPlayerPtrAddr == 0` (offsets unconfirmed). This is the one real blocker.
- **Input injection — works.** `ge_inject_keyboard()` (ge_hooks.cpp) runs every
  controller poll on the game thread and OR-s synthetic buttons into the slot-0
  gamepad buffer at `GE_PAD0 = 0x830C8B9C`. Keybinds are cvars parsed via
  `ge_key_down(name)`.
- **Scrollwheel — already pollable.** The ReXGlue SDK maps wheel detents to
  synthetic virtual keys `kMouseWheelUp`/`kMouseWheelDown`, named `"WheelUp"` /
  `"WheelDown"` in `keybinds.cpp`, held asserted for a few frames, flowing
  through the same key-down table `ge_key_down` reads.
- **Discovery diagnostic — exists.** `kProbeAddr`/`kProbeLen` in ge_gamestate.cpp
  log a hex window of guest memory whenever it changes, for locking in offsets
  on-device by watching what moves as weapons cycle.

## Components

### 1. Actuation — confirm the guest memory write path (`ge_gamestate.cpp`)

The gating piece. Everything else is inert without it. Two phases:

1. **Discover the write field.** Enable the `kProbeAddr` diagnostic and cycle
   weapons using the game's native controller input. Identify the field the game
   *reads to initiate* a switch. This is expected to be distinct from
   `0x447f10b0`, which is the equipped-id **mirror** the game re-writes every
   frame — poking that alone will likely be overwritten next frame and/or skip
   the game's draw-animation/ammo logic. We want the "requested weapon" input
   field (the same one a native weapon-cycle input sets).
2. **Wire it in.** Set the confirmed constants: `kEquipRequestOff` plus either
   `kPlayerPtrAddr` (pointer-chase path) or a fixed direct address (like the
   read path's `0x447f10b0`), and optionally the `kStateOff` / `kStateInPlayMask`
   in-play safety gate. No logic changes to `apply_equip()` /
   `RequestEquipWeapon()` — they are already correct; they only need real
   offsets.

**Fallback if the request field is hard to isolate:** try writing `0x447f10b0`
directly and observe whether the game honors it as a switch. Document whichever
mechanism wins.

**Bonus:** once actuation is live, the existing Thor second-screen taps (which
already call `RequestEquipWeapon`) begin switching weapons with no extra work.

### 2. Desktop input driver (`ge_hooks.cpp`, controller-poll path)

A per-frame poll on the game thread (alongside / within the existing
`ge_inject_keyboard` flow):

- **Scrollwheel:** `ge_key_down("ge_key_wpn_next")` / `"ge_key_wpn_prev")`
  (default binds `"WheelUp"` / `"WheelDown"`) → step to the next / previous entry
  in `held_ids` relative to the current `equipped_id`, wrapping around →
  `RequestEquipWeapon(held_ids[newIndex])`.
- **Number keys 1–9:** select the Nth entry in `held_ids` (dense, same order the
  second screen uses) → `RequestEquipWeapon(held_ids[n-1])` when `n <=
  held_count`.
- **Rising-edge detection:** wheel detents assert for several frames and number
  keys auto-repeat, so track the previous-frame down state per input and fire
  only on the down transition — exactly one weapon change per detent / press.
- **New cvars:** `ge_key_wpn_next` (`"WheelUp"`), `ge_key_wpn_prev`
  (`"WheelDown"`), and an enable toggle (e.g. `ge_weapon_select_enable`).
  Digits 1–9 are read directly from the listener.
- Gated by `ge_input_active()` like the rest of the keyboard input, and a no-op
  when the snapshot is invalid or `held_count == 0`.

### 3. Desktop overlay — numbered HUD (`ge_weaponmenu.*` or a sibling)

A small ImGui overlay on the **main** window, gated by a cvar (e.g.
`ge_weapon_overlay`), drawing the carried weapons as a numbered list
(`1: PP7`, `2: KF7 Soviet`, …) with the equipped weapon highlighted. Reads
`GetWeaponSnapshot()` and reuses `WeaponLabel()`. `WeaponLabel` is factored so
the overlay and the existing `WeaponMenuDialog` share one id→name map instead of
duplicating it. Hidden when the snapshot is invalid or no weapons are carried.

## Data flow

```
game thread: OnFrame -> publish WeaponSnapshot            (existing)
game thread: input driver -> read snapshot, compute target,
                             RequestEquipWeapon(target)    (new, edge-triggered)
game thread: apply_equip -> write guest memory            (existing, newly wired)
UI thread:   overlay -> GetWeaponSnapshot() -> draw        (new)
```

All three thread interactions already use the existing lock-free seqlock /
atomic request primitives; no new synchronization is introduced.

## Error handling

- Invalid snapshot or `held_count == 0` → input driver no-ops; overlay hidden.
- `apply_equip()` keeps its existing guards: valid player, in-play state gate,
  and `held_mask` membership (never switch to a weapon not carried).
- Number key beyond `held_count` → ignored.

## Testing

Primarily manual on desktop (guest-memory actuation is not unit-testable):

1. Verify the write field with the `kProbeAddr` diagnostic before wiring.
2. Build `ge` (linux preset), load a level, pick up multiple weapons.
3. Scrollwheel up/down cycles weapons; overlay + GoldenEye's own HUD agree.
4. Number keys jump to the Nth carried weapon.
5. Confirm no switch during menus/cutscenes/death (state gate holds).

## Scope / YAGNI

- **Desktop only.** No pending-selection cursor — immediate switch per input.
- Number keys map to **held-list position**, not fixed weapon ids.
- Thor second-screen input/overlay deferred (actuation benefits it regardless).
- No new tests infrastructure; manual desktop verification + the diagnostic.
