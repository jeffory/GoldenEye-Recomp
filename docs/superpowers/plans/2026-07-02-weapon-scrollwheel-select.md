# Desktop Weapon Selection (Scrollwheel + Numbers) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let a desktop player switch GoldenEye weapons with the mouse scrollwheel (next/prev) and number keys 1–9 (jump to the Nth carried weapon), with a numbered on-screen overlay, by wiring the currently-inert guest-memory equip-write path.

**Architecture:** Reuse the dual-screen bridge (`gamestate::WeaponSnapshot` / `RequestEquipWeapon` / `apply_equip`). Task 1 confirms and wires the guest-memory field that actually switches the weapon (the one gating unknown). Task 2 adds an edge-triggered desktop input driver in the existing controller-poll hook. Task 3 adds a passive numbered overlay modeled on `FpsOverlay`. Task 4 exposes an in-game menu toggle.

**Tech Stack:** C++17, ImGui, ReXGlue SDK (`rex::cvar`, `rex::ui` input listener), CMake presets.

## Global Constraints

- **Platform for this plan: desktop/Linux only.** Thor second-screen input/UI is out of scope (though Task 1 makes the existing second-screen taps start working as a free side effect).
- **`ge_weapon_select_enable` defaults ON; `ge_weapon_overlay` defaults OFF.**
- **Number keys map to held-list position** (`held_ids[n-1]`), not fixed weapon ids.
- **Immediate switch per input** — no pending-selection cursor.
- **No new test framework.** Guest-memory + ImGui behavior is verified manually on desktop; each task ends with a build + run + observe cycle.
- **Reuse `WeaponLabel`** — one id→name map shared by the second-screen dialog and the new overlay; never duplicate it.
- **Follow existing patterns:** cvars via `REXCVAR_DEFINE_*`; keybinds parsed with `ParseVirtualKey`/`g_listener.key_down`; overlays created in `GeApp::OnCreateDialogs` and torn down in `OnShutdown`.
- **Build:** `cmake --build --preset linux-amd64-relwithdebinfo --target ge`
- **Run:** `LD_LIBRARY_PATH=../GoldenEye-Recomp-rexglue/out/linux-amd64 ./out/build/linux-amd64-relwithdebinfo/ge --game_data_root=$PWD/assets` (append cvar flags like `--log_level debug`). Confirm the exact `ge` binary path with `find out -name ge -type f -executable` after the first build.
- **Commit** after every task with the trailer `Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>`.

---

## Task 1: Confirm & wire the guest weapon-switch write path

This is the gating unknown. The write path in `apply_equip()` is inert because
`kEquipRequestOff`/`kPlayerPtrAddr` are `0`. We first try the cheap hypothesis
(write the equipped-id mirror `0x447f10b0` directly); if the game ignores it, we
fall back to diffing memory to find the real "requested weapon" field. A
temporary debug keybind drives equips so this task verifies independently of the
later input driver.

**Files:**
- Modify: `src/ge_gamestate.cpp` (add a direct-write branch in `apply_equip`; set discovered constants; toggle the probe)
- Modify: `src/ge_hooks.cpp` (temporary debug keybind — removed in Task 2)

**Interfaces:**
- Consumes: `gamestate::GetWeaponSnapshot()`, `gamestate::RequestEquipWeapon(int32_t)`, `apply_equip(void*, uint8_t*, const WeaponSnapshot&)`, `ST32(base,addr,val)`, `LD32(base,addr)`.
- Produces: a **live** equip path — after this task `RequestEquipWeapon(id)` for a held weapon visibly switches the in-game weapon. Introduces constant `kEquipIdWriteAddr` (fixed guest address written on equip; `0` if the pointer-path was used instead).

- [ ] **Step 1: Add a direct-write branch to `apply_equip` (primary hypothesis)**

In `src/ge_gamestate.cpp`, add a constant next to the other DIRECT-path
addresses (near line 165):

```cpp
// Equip WRITE via fixed address (mirror of the read path). If the game honors a
// write to the equipped-id block as a switch, this is the whole write path and
// no player-pointer chase is needed. 0 => disabled (use the pointer path below).
inline constexpr uint32_t kEquipIdWriteAddr = 0x447f10b0u;  // TRY FIRST
```

In `apply_equip()` (near line 664, after the `req` is decoded and the
`id` range/held_mask guards at lines 666–667), insert the direct-write branch
*before* the existing pointer-path block:

```cpp
  // Primary: direct fixed-address write (no pointer chase). Confirmed/kept only
  // if the game actually switches when this field is written.
  if (kEquipIdWriteAddr != 0u) {
    ST32(base, kEquipIdWriteAddr, static_cast<uint32_t>(id));
    return;
  }
```

- [ ] **Step 2: Add a temporary debug keybind to trigger equips**

In `src/ge_hooks.cpp`, at the end of `ge_inject_keyboard()` (after line 1496,
before the closing brace at 1497), add a throwaway cycle trigger. This is
removed in Task 2.

```cpp
  // TEMP (Task 1 verification, removed in Task 2): press N to cycle to the next
  // carried weapon via the bridge, so the write path can be validated on its own.
  {
    static bool prev_n = false;
    const bool n = g_listener.key_down(rex::ui::VirtualKey::kN);
    if (n && !prev_n) {
      const auto snap = ge::gamestate::GetWeaponSnapshot();
      if (snap.valid && snap.held_count > 0) {
        int idx = 0;
        for (int i = 0; i < snap.held_count; ++i)
          if (snap.held_ids[i] == snap.equipped_id) { idx = i; break; }
        const int next = (idx + 1) % snap.held_count;
        ge::gamestate::RequestEquipWeapon(snap.held_ids[next]);
        REXKRNL_INFO("GEWPN debug cycle -> id %d", snap.held_ids[next]);
      }
    }
    prev_n = n;
  }
```

Confirm `VirtualKey::kN` exists (`grep -n "kN " ../GoldenEye-Recomp-rexglue/include/rex/ui/virtual_key.h`); if the enum name differs, use the matching letter constant. Ensure `#include "ge_gamestate.h"` is present in ge_hooks.cpp (`grep -n ge_gamestate.h src/ge_hooks.cpp`); add it if missing.

- [ ] **Step 3: Build**

Run: `cmake --build --preset linux-amd64-relwithdebinfo --target ge`
Expected: compiles cleanly (relinks `librexruntimerd.so` + `ge`).

- [ ] **Step 4: Run and test the direct-write hypothesis**

Run the game (see Global Constraints run line, add `--log_level debug`), start a
level, pick up at least one extra weapon, then press **N** repeatedly.

Expected (success case): the in-game equipped weapon changes each press, and
`GEWPN debug cycle -> id N` lines appear in the log.

- [ ] **Step 5: If the direct write did NOT switch the weapon — find the real field**

Only if Step 4 failed. Enable the discovery probe in `src/ge_gamestate.cpp`:

```cpp
inline constexpr uint32_t kProbeAddr = 0x447f10a0u;  // window spanning the equip block
inline constexpr uint32_t kProbeLen  = 128u;
```

Rebuild, run, and trigger a **native** weapon switch to see which bytes move
when the game switches on its own:
- If you have a USB/Bluetooth gamepad, press the game's weapon-switch button.
- If keyboard-only, temporarily sweep candidate buttons by OR-ing one at a time
  into `GE_PAD0` in `ge_inject_keyboard` (e.g. bind a key to `BTN_Y`, then
  `BTN_DPAD_UP`, etc.) until the weapon changes; that identifies the native
  switch button.

Watch the `GEPROBE`-style hex dumps in the log: the field that changes to the
*target* id one frame **before** `0x447f10b0` updates is the request field.
Record its guest address (fixed) or, if it lives in the player struct, the base
pointer address + offset.

- [ ] **Step 6: If Step 5 was needed — wire the pointer/request path**

Set `kEquipIdWriteAddr = 0u` (disable the direct write) and fill in the
discovered constants in `src/ge_gamestate.cpp`:

```cpp
inline constexpr uint32_t kPlayerPtrAddr   = 0x________u;  // discovered player-struct pointer
inline constexpr uint32_t kEquipRequestOff = 0x____u;      // discovered request-field offset
// Optional in-play gate, only if a clean state field was found:
inline constexpr uint32_t kStateOff        = 0x____u;
inline constexpr uint32_t kStateInPlayMask = 0x________u;
```

If instead the request field is a *fixed* address (no pointer chase), set
`kEquipIdWriteAddr` to that address and keep the pointer constants `0`. Turn the
probe back off (`kProbeAddr = 0u`). Rebuild and repeat Step 4 until pressing N
switches weapons.

- [ ] **Step 7: Commit**

```bash
git add src/ge_gamestate.cpp src/ge_hooks.cpp
git commit -m "feat(ds): wire the guest weapon-switch write path (equip actuation)

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

## Task 2: Desktop input driver — scrollwheel + number keys

Replace the temporary trigger with the real edge-triggered driver: scrollwheel
cycles, digits 1–9 jump, gated by a cvar.

**Files:**
- Modify: `src/ge_hooks.cpp` (remove the Task 1 temp block; add cvars + the driver)

**Interfaces:**
- Consumes: `gamestate::GetWeaponSnapshot()`, `gamestate::RequestEquipWeapon(int32_t)`, `ge_key_down(const char*)`, `g_listener.key_down(VirtualKey)`, `ge_input_active()`, `REXCVAR_GET`.
- Produces: a per-frame `ge_weapon_select_poll()` behavior invoked from `ge_inject_keyboard`; new cvars `ge_weapon_select_enable`, `ge_key_wpn_next`, `ge_key_wpn_prev`.

- [ ] **Step 1: Remove the Task 1 temporary debug block**

Delete the `// TEMP (Task 1 verification ...)` block added to
`ge_inject_keyboard()` in Task 1 Step 2.

- [ ] **Step 2: Add cvars**

In `src/ge_hooks.cpp`, next to the other input cvars (near line 1411–1431):

```cpp
REXCVAR_DEFINE_BOOL(ge_weapon_select_enable, true, "Input",
                    "Desktop weapon selection: scrollwheel cycles, number keys pick");
REXCVAR_DEFINE_STRING(ge_key_wpn_next, "WheelUp", "Input/Keybinds",
                      "Next weapon (scroll up)");
REXCVAR_DEFINE_STRING(ge_key_wpn_prev, "WheelDown", "Input/Keybinds",
                      "Previous weapon (scroll down)");
```

- [ ] **Step 3: Add the driver helper**

In `src/ge_hooks.cpp`, in the anonymous namespace that holds `ge_key_down`
(near line 1408), add:

```cpp
// Desktop weapon selection. Edge-triggered so one detent / keypress = one
// switch. Steps through the carried-weapon list relative to the equipped id, or
// jumps to the Nth carried weapon for digits 1-9. No-op without a valid snapshot.
void ge_weapon_select_poll() {
  if (!REXCVAR_GET(ge_weapon_select_enable) || !ge_input_active()) return;

  const auto snap = ge::gamestate::GetWeaponSnapshot();
  if (!snap.valid || snap.held_count == 0) return;

  // Current index within the dense held list (default 0 if equipped not found).
  int cur = 0;
  for (int i = 0; i < snap.held_count; ++i)
    if (snap.held_ids[i] == snap.equipped_id) { cur = i; break; }

  // Scrollwheel: rising-edge next/prev with wrap.
  static bool prev_next = false, prev_prev = false;
  const bool nx = ge_key_down("ge_key_wpn_next");
  const bool pv = ge_key_down("ge_key_wpn_prev");
  int target = -1;
  if (nx && !prev_next) target = snap.held_ids[(cur + 1) % snap.held_count];
  else if (pv && !prev_prev)
    target = snap.held_ids[(cur + snap.held_count - 1) % snap.held_count];
  prev_next = nx; prev_prev = pv;

  // Digits 1-9: rising-edge jump to the Nth carried weapon (VirtualKey::k1==0x31).
  static bool prev_digit[9] = {};
  for (int n = 1; n <= 9; ++n) {
    const auto vk = static_cast<rex::ui::VirtualKey>(0x31 + (n - 1));
    const bool down = g_listener.key_down(vk);
    if (down && !prev_digit[n - 1] && n <= snap.held_count)
      target = snap.held_ids[n - 1];
    prev_digit[n - 1] = down;
  }

  if (target >= 0 && target != snap.equipped_id)
    ge::gamestate::RequestEquipWeapon(target);
}
```

- [ ] **Step 4: Call the driver each frame**

In `ge_inject_keyboard()`, immediately after the keyboard early-return line
`if (!REXCVAR_GET(ge_keyboard_enable) || !ge_input_active()) return;` (line 1468)
— add the call *before* that return so weapon-select works even if the general
keyboard mapping is off. Place it right after the mouse-look block (after line
1466):

```cpp
  ge_weapon_select_poll();
```

- [ ] **Step 5: Build**

Run: `cmake --build --preset linux-amd64-relwithdebinfo --target ge`
Expected: compiles cleanly.

- [ ] **Step 6: Run and verify**

Start a level, pick up 2+ weapons. Expected:
- Scroll up/down cycles to the next/previous carried weapon (one per detent).
- Pressing `1`,`2`,… selects the 1st, 2nd, … carried weapon.
- No change when scrolling with only one weapon carried; digits beyond the
  carried count do nothing.

- [ ] **Step 7: Commit**

```bash
git add src/ge_hooks.cpp
git commit -m "feat(ds): desktop scrollwheel + number-key weapon selection

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

## Task 3: Numbered bottom-left overlay + shared WeaponLabel

Factor the weapon-name map into a shared unit, then add a passive numbered
overlay on the main window modeled on `FpsOverlay`.

**Files:**
- Create: `src/ge_weapons.h`, `src/ge_weapons.cpp` (shared `WeaponLabel`)
- Create: `src/ge_weaponoverlay.h`, `src/ge_weaponoverlay.cpp` (the overlay)
- Modify: `src/ge_weaponmenu.cpp` (use shared `WeaponLabel`)
- Modify: `src/ge_weaponmenu.h` (drop the private `WeaponLabel` declaration)
- Modify: `src/ge_app.h` (create/own/tear down the overlay like `fps_overlay_`)
- Modify: `CMakeLists.txt` (add the two new `.cpp` files)

**Interfaces:**
- Consumes: `gamestate::GetWeaponSnapshot()`, `rex::ui::ImGuiDialog`, `rex::ui::ImGuiDrawer`, `REXCVAR_GET`.
- Produces: `const char* ge::WeaponLabel(int id)`; class `ge::WeaponOverlay : rex::ui::ImGuiDialog` (ctor `explicit WeaponOverlay(rex::ui::ImGuiDrawer*)`); cvar `ge_weapon_overlay`.

- [ ] **Step 1: Create the shared label unit**

`src/ge_weapons.h`:

```cpp
// ge - shared weapon id -> display name map. Used by both the second-screen
// weapon menu (ge_weaponmenu) and the desktop overlay (ge_weaponoverlay).
#pragma once
namespace ge {
// Human-friendly name for a confirmed weapon id; "Weapon N" for unknown ids.
const char* WeaponLabel(int id);
}  // namespace ge
```

`src/ge_weapons.cpp` — move the body verbatim from `WeaponMenuDialog::WeaponLabel`
(ge_weaponmenu.cpp lines 39–63):

```cpp
#include "ge_weapons.h"
#include <cstdio>

namespace ge {
const char* WeaponLabel(int id) {
  switch (id) {
    case 1:  return "Unarmed";
    case 4:  return "PP7 (Unsilenced)";
    case 5:  return "PP7";
    case 6:  return "DD44";
    case 7:  return "Klobb";
    case 8:  return "KF7 Soviet";
    case 17: return "Sniper Rifle";
    case 24: return "Grenade Launcher";
    case 26: return "Hand Grenade";
    case 27: return "Timed Mine";
    case 29: return "Remote Mine";
    case 30: return "Detonator";
    default: break;
  }
  static char buf[16];
  std::snprintf(buf, sizeof(buf), "Weapon %d", id);
  return buf;
}
}  // namespace ge
```

- [ ] **Step 2: Point the second-screen menu at the shared label**

In `src/ge_weaponmenu.h`, delete the private `static const char* WeaponLabel(int id);`
declaration (lines 34–38). In `src/ge_weaponmenu.cpp`, delete the
`WeaponMenuDialog::WeaponLabel` definition (lines 39–63), add
`#include "ge_weapons.h"` near the other includes, and replace the two
`WeaponLabel(id)` call sites (lines 154, 157) with `ge::WeaponLabel(id)` (or
just `WeaponLabel(id)` since the file is in `namespace ge`).

- [ ] **Step 3: Create the overlay class**

`src/ge_weaponoverlay.h`:

```cpp
// ge - passive numbered weapon overlay on the PRIMARY (main) window. Mirrors the
// carried-weapon list for desktop scrollwheel/number selection. Created once at
// startup like FpsOverlay; gated each frame by the ge_weapon_overlay cvar.
#pragma once
#include <rex/ui/imgui_dialog.h>
namespace ge {
class WeaponOverlay final : public rex::ui::ImGuiDialog {
 public:
  explicit WeaponOverlay(rex::ui::ImGuiDrawer* drawer);
  ~WeaponOverlay() override;
 protected:
  void OnDraw(ImGuiIO& io) override;
};
}  // namespace ge
```

`src/ge_weaponoverlay.cpp`:

```cpp
#include "ge_weaponoverlay.h"

#include "ge_gamestate.h"
#include "ge_weapons.h"

#include <rex/cvar.h>

#include <imgui.h>

#include <cstdio>

// Numbered carried-weapon overlay, bottom-left of the main window. Default OFF;
// toggle with --ge_weapon_overlay=true or the pause-menu checkbox.
REXCVAR_DEFINE_BOOL(ge_weapon_overlay, false, "Debug",
                    "GoldenEye: draw a numbered carried-weapon overlay (desktop "
                    "scrollwheel/number selection)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace ge {

WeaponOverlay::WeaponOverlay(rex::ui::ImGuiDrawer* drawer)
    : rex::ui::ImGuiDialog(drawer) {}
WeaponOverlay::~WeaponOverlay() = default;

void WeaponOverlay::OnDraw(ImGuiIO& io) {
  if (!REXCVAR_GET(ge_weapon_overlay)) return;

  const auto snap = gamestate::GetWeaponSnapshot();
  if (!snap.valid || snap.held_count == 0) return;

  // Pin to bottom-left with a small margin; auto-size to contents.
  const float margin = 12.0f;
  ImGui::SetNextWindowPos(ImVec2(margin, io.DisplaySize.y - margin),
                          ImGuiCond_Always, ImVec2(0.0f, 1.0f));
  ImGui::SetNextWindowBgAlpha(0.55f);
  ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                           ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                           ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoInputs |
                           ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing |
                           ImGuiWindowFlags_AlwaysAutoResize;
  if (ImGui::Begin("##ge_weapon_overlay", nullptr, flags)) {
    ImGui::SetWindowFontScale(1.5f);
    for (int i = 0; i < snap.held_count; ++i) {
      const int id = snap.held_ids[i];
      const bool equipped = (id == snap.equipped_id);
      if (equipped)
        ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.25f, 1.0f), "%d: %s  <", i + 1,
                           ge::WeaponLabel(id));
      else
        ImGui::Text("%d: %s", i + 1, ge::WeaponLabel(id));
    }
  }
  ImGui::End();
}

}  // namespace ge
```

- [ ] **Step 4: Register the new sources in CMake**

In `CMakeLists.txt`, add after line 29 (`src/ge_weaponmenu.cpp`):

```cmake
    src/ge_weapons.cpp          # shared weapon id->name map
    src/ge_weaponoverlay.cpp    # primary-window numbered weapon overlay
```

- [ ] **Step 5: Create, own, and tear down the overlay in GeApp**

In `src/ge_app.h`, add `#include "ge_weaponoverlay.h"` with the other ge includes.
In `OnCreateDialogs` (after line 119, `fps_overlay_ = ...`):

```cpp
    weapon_overlay_ = std::make_unique<ge::WeaponOverlay>(drawer);
```

In `OnShutdown` (after line 144, `fps_overlay_.reset();`):

```cpp
    weapon_overlay_.reset();
```

Add the member next to `fps_overlay_` (find it: `grep -n "fps_overlay_" src/ge_app.h`):

```cpp
  std::unique_ptr<ge::WeaponOverlay> weapon_overlay_;
```

- [ ] **Step 6: Build**

Run: `cmake --build --preset linux-amd64-relwithdebinfo --target ge`
Expected: compiles and links (new files picked up by CMake — a clean configure
runs automatically via the preset).

- [ ] **Step 7: Run and verify**

Run with `--ge_weapon_overlay=true`. Start a level, pick up weapons. Expected: a
bottom-left list `1: PP7`, `2: KF7 Soviet`, … with the equipped weapon shown in
gold with a `<` marker; it updates as you scroll/press digits (Task 2). Run
without the flag: no overlay. Confirm the second-screen menu still shows correct
names (shared `WeaponLabel`).

- [ ] **Step 8: Commit**

```bash
git add src/ge_weapons.h src/ge_weapons.cpp src/ge_weaponoverlay.h src/ge_weaponoverlay.cpp src/ge_weaponmenu.h src/ge_weaponmenu.cpp src/ge_app.h CMakeLists.txt
git commit -m "feat(ds): numbered bottom-left weapon overlay + shared WeaponLabel

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

## Task 4: In-game menu toggle for the overlay

Expose `ge_weapon_overlay` as a checkbox in the pause menu, next to the existing
second-screen weapon-menu checkbox.

**Files:**
- Modify: `src/ge_menu.cpp` (add the checkbox)

**Interfaces:**
- Consumes: `GetCvarB(const char*)`, `SetCvarB(const char*, bool)`, `ImGui::Checkbox`, cvar `ge_weapon_overlay`.
- Produces: nothing consumed by later tasks.

- [ ] **Step 1: Add the checkbox**

In `src/ge_menu.cpp`, immediately after the existing second-screen weapon-menu
checkbox block (lines 553–555):

```cpp
      bool wpn_overlay = GetCvarB("ge_weapon_overlay");
      if (ImGui::Checkbox("Weapon overlay (numbered list)", &wpn_overlay)) {
        SetCvarB("ge_weapon_overlay", wpn_overlay);
      }
```

- [ ] **Step 2: Build**

Run: `cmake --build --preset linux-amd64-relwithdebinfo --target ge`
Expected: compiles cleanly.

- [ ] **Step 3: Run and verify**

Start the game, open the pause menu (Escape), find the checkbox on the same tab
as "Weapon menu on second screen". Toggling it shows/hides the overlay live
(the cvar is hot-reloadable). Verify it persists if "Save" is used.

- [ ] **Step 4: Commit**

```bash
git add src/ge_menu.cpp
git commit -m "feat(ds): pause-menu toggle for the numbered weapon overlay

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

## Self-Review Notes

- **Spec coverage:** actuation/write-path → Task 1; scrollwheel + numbers + edge
  detection + cvars → Task 2; bottom-left numbered overlay (default off) + shared
  `WeaponLabel` → Task 3; in-game menu toggle → Task 4. Defaults
  (`ge_weapon_select_enable` on, `ge_weapon_overlay` off), held-list number
  mapping, and immediate-switch are all captured.
- **Type consistency:** `ge::WeaponLabel(int)`, `WeaponOverlay(rex::ui::ImGuiDrawer*)`,
  `RequestEquipWeapon(int32_t)`, and the `WeaponSnapshot` fields
  (`valid`, `held_count`, `held_ids`, `equipped_id`) are used identically across
  tasks and match the existing headers.
- **Known exploratory step:** Task 1 Steps 5–6 discover concrete guest offsets
  that cannot be known until run on the machine; the procedure and acceptance
  criterion (pressing N switches the weapon) are explicit. This is inherent to
  the reverse-engineering, not a placeholder.
```
