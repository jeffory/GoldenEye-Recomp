# Dual-screen game-state bridge (carried weapons / ammo + active-weapon switch)

Part of **HOM-156** dual-screen support. This bridge is the data boundary
between the running game and the second-screen weapon-selection menu: it reads
the player's carried weapons, equipped weapon, and ammo out of guest memory and
publishes a thread-safe snapshot the UI thread can consume, and it takes equip
requests back from the UI and applies them on the game thread.

Files:
- `src/ge_gamestate.h` — public API (no PPC/generated headers; cheap to include).
- `src/ge_gamestate.cpp` — read path, write path, seqlock, discovery diagnostic.
- Pumped once per displayed frame from `ge_diag_vdswap` in `src/ge_hooks.cpp`
  (the guest present hook at `sub_821996F8`).

## API (consumed by the weapon-menu UI task)

```cpp
#include "ge_gamestate.h"
using namespace ge::gamestate;

WeaponSnapshot s = GetWeaponSnapshot();   // any thread, lock-free, never blocks
if (s.valid) {
  // s.equipped_id           current weapon id  (kNoWeapon if none)
  // s.held_mask             bit i set => weapon id i is carried
  // s.held_ids[0..count)    dense list of carried weapon ids
  // s.ammo[id]              ammo for weapon id (valid when held_mask has bit id)
  // s.frame                 producer frame counter (detect a stalled producer)
}

RequestEquipWeapon(id);   // any thread; applied on the next game frame, safely
```

Thread-safety:
- `GetWeaponSnapshot()` returns a torn-free copy via a single-writer **seqlock**.
  Safe from the UI/menu thread; never touches guest memory.
- `RequestEquipWeapon()` posts an atomic, coalesced request (last-writer-wins
  before the next frame). The game thread applies it from `OnFrame`, and only
  when a valid, in-play player exists — never during a menu/cutscene/dead state.
- `OnFrame()` runs on the **game thread only** (the present hook). It is the sole
  reader/writer of guest inventory memory.

## Read / write mechanism

Guest memory is big-endian PPC, accessed with `getcb` + `LD8/LD16/LD32` /
`ST16/ST32` (same helpers as `ge_hooks.cpp`). The read path resolves a pointer
to the player/inventory struct from a fixed guest global, then reads the
equipped id, the carried-weapon set, and per-slot ammo. The write path writes the
game's per-frame weapon-select field (`ST32`) — the same field a weapon-cycle
input sets — so the game runs its own switch logic (draw animation, ammo check)
instead of us forcing inconsistent state. Switching to a weapon the player does
not hold, or during an unsafe state, is rejected.

## Guest layout constants — **NEEDS ON-DEVICE CONFIRMATION**

This checkout contains **no game binary, symbol map, or IDA database**, and the
target is the AYN Thor (Android arm64) — the offsets cannot be empirically
verified from source alone. They are isolated in one block at the top of
`ge_gamestate.cpp`:

| constant            | meaning                                              |
|---------------------|------------------------------------------------------|
| `kPlayerPtrAddr`    | guest addr of the pointer to the player/inventory struct |
| `kEquippedIdOff`    | offset to the 32-bit equipped weapon id              |
| `kHeldMaskOff`      | offset to the 32-bit carried-weapon bitmask (0 ⇒ derive from ammo) |
| `kAmmoBaseOff`/`kAmmoStride`/`kNumSlots` | per-slot ammo array layout      |
| `kEquipRequestOff`  | per-frame weapon-select field written to switch weapon |
| `kStateOff`/`kStateInPlayMask` | optional "alive & in normal play" safety gate |

**Until `kPlayerPtrAddr` is set to a confirmed value the bridge is INERT**:
`OnFrame` publishes `valid=false` and drops equip requests, so the game is
completely unaffected. This is the intended, safe default for merging now.

### Confirming the constants (the acceptance demo)

Follow the project's established diagnostic-driven RE method (the same approach
the freeze watchdog / `ge_dbg_now` used to reverse the GPU pipeline):

1. Build with the game files. Enable the `ge_gamestate_diag` cvar. Set
   `kProbeAddr`/`kProbeLen` to a candidate region.
2. In-game, cycle weapons and pick up ammo while watching the change log
   (`GEGAMESTATE probe @… : …`). The bytes that move in lockstep with the
   equipped weapon and ammo are your offsets; anchor the player-struct pointer
   the same way.
3. Cross-reference public GoldenEye 007 RE notes (the carried-weapon / ammo
   layout the XBLA build inherits from the N64 original) to label fields.
4. Fill the constants, rebuild, and verify: the snapshot tracks the real
   equipped weapon as you switch in-game, and `RequestEquipWeapon()` actually
   switches it. Record the confirmed offsets in the table above.

## Branch

Implemented on `ds/game-state-bridge`, merged into the dual-screen integration
branch `feat/ds-support` (repo: `GoldenEye-Recomp`).
