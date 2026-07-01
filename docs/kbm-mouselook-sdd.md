# SDD: Keyboard & Mouse (mouselook) support for GoldenEye Recomp — Windows/Linux

> Spec-Driven Development plan. Phases 0–2 (investigation → spec → design) are the
> primary deliverable; Phases 3–4 (implementation → verification) describe the path
> once the approach is confirmed.
>
> Scope note: this document concerns **GoldenEye Recomp** (the Xbox 360 / XBLA
> static recompilation built on the ReXGlue SDK) — not an N64 recomp. See
> §0.0 for why that distinction changes the entire input model.

---

## Phase 0 — Investigation

### 0.0 Platform correction (important)

The issue framed GoldenEye Recomp as an **N64** recompilation in the
N64Recomp / Zelda64Recomp family, with an "N64 input contract" (C-buttons,
analog stick, Z-trigger). **It is not.** GoldenEye Recomp statically recompiles
the **Xbox 360 XBLA** build of GoldenEye into native C++ via the **ReXGlue SDK**
(a Xenia-derived PowerPC→C++ recompiler, lineage: Xenia → XenonRecomp →
ReXGlue). README: *"A native PC port of GoldenEye 007 (Xbox 360 / XBLA), built
by statically recompiling the original game into C++ with the ReXGlue SDK."*

Consequences for input:

- The game reads input through the **XInput / XAM** API, not an N64 controller
  struct. The contract is `X_INPUT_GAMEPAD` (`rexglue/include/rex/input/input.h`):
  16-bit `buttons` bitfield, two 8-bit triggers, two 16-bit thumbstick axis
  pairs (`thumb_lx/ly`, `thumb_rx/ry`).
- Aiming in GoldenEye XBLA is **dual-analog** (right stick = look). There is no
  "analog-stick → mouselook" remap to design at the *game* level; the game
  already expects a right stick. Our job is to make a **mouse drive that right
  stick** convincingly.
- Because the game is a recompiled closed binary, we **cannot cheaply reach into
  the game's internal aim/camera variables** the way a decompilation can. All
  input must flow through the `X_INPUT_GAMEPAD` struct that
  `XamInputGetState` returns. This is the single most important constraint in
  this document (see §0.3 contrast with Perfect Dark, and §2.2).

### 0.1 How input flows today

The runtime lives in the ReXGlue SDK fork (`GoldenEye-Recomp-rexglue`); the
game-side repo (`GoldenEye-Recomp`) only adds menus/hooks/post-FX and does **not**
implement input. The chain, from key press to game logic:

```
OS window event (Win32 / GTK / Android)
  → rex::ui::Window dispatches to WindowInputListener (OnKeyDown/Up, OnMouse*)
    → rex::input::InputSystem holds a list of InputDriver instances
      → each driver fills an X_INPUT_GAMEPAD on GetState()
        → InputSystem::GetState() MERGES all drivers into one gamepad
          → XamInputGetState_entry (kernel/xam/xam_input.cpp)
            → recompiled game reads the X_INPUT_STATE each frame
```

Key files (all in `GoldenEye-Recomp-rexglue`):

| Area | File |
| --- | --- |
| XInput data contract | `include/rex/input/input.h` (`X_INPUT_GAMEPAD`, `X_INPUT_STATE`, button enums) |
| Driver interface | `include/rex/input/input_driver.h` (`GetCapabilities/GetState/SetState/GetKeystroke`) |
| Aggregator + factory | `src/input/input_system.cpp` (`CreateDefaultInputSystem`, merge logic) |
| **MnK driver (already exists)** | `src/input/mnk/mnk_input_driver.cpp` + header |
| SDL gamepad driver | `src/input/sdl/sdl_input_driver.cpp` |
| XInput (native, Win32) driver | `src/input/xinput/xinput_input_driver.cpp` |
| Game-facing kernel entry | `src/kernel/xam/xam_input.cpp` (`XamInputGetState_entry`) |
| Window base / capture API | `src/ui/window.cpp`, `include/rex/ui/window.h` |
| Win32 window (capture impl) | `src/ui/window_win.cpp` |
| **GTK window (Linux)** | `src/ui/window_gtk.cpp` |
| cvar config (persist) | `src/core/cvar.cpp` (`LoadConfig`/`SaveConfig`, TOML) |
| Keybind parser / registry | `src/ui/keybinds.cpp`, `include/rex/ui/keybinds.h` |
| Virtual-key enum | `include/rex/ui/virtual_key.h` |
| Settings/rebind UI | `src/ui/overlay/settings_overlay.cpp` |

### 0.2 The N64/XInput input contract (what the game reads each frame)

`X_INPUT_GAMEPAD` (`include/rex/input/input.h`):

```c
struct X_INPUT_GAMEPAD {
  be<uint16_t> buttons;      // X_INPUT_GAMEPAD_A/B/X/Y, DPAD_*, START/BACK,
                             // LEFT/RIGHT_SHOULDER, LEFT/RIGHT_THUMB, GUIDE
  uint8_t      left_trigger; // 0..255
  uint8_t      right_trigger;// 0..255
  be<int16_t>  thumb_lx, thumb_ly;  // -32768..32767  (movement)
  be<int16_t>  thumb_rx, thumb_ry;  // -32768..32767  (look/aim)
};
```

`XamInputGetState_entry` → `InputSystem::GetState(user_index, &state)` populates
exactly this. There is no other input seam the game observes.

### 0.3 What already exists — and where it works

**KB/M is already substantially implemented** in `MnkInputDriver`. It is wired
into the default device list (`CreateDefaultInputSystem` adds it after the
SDL/XInput gamepad driver, before the NOP fallback), so it ships in the game
today. What it already does:

- **Keyboard → gamepad**: every gamepad button + both sticks (as digital
  WASD-style deflection to ±`INT16_MAX`) are bound through cvars
  (`keybind_a`, `keybind_lstick_up`, …) with sensible FPS defaults
  (W/A/S/D move, Space = A/jump, LMB → right trigger = fire, RMB → left trigger
  = aim, E/R/Q/F = Y/X/LB/RB, Esc = Start, Tab = Back).
- **Mouse buttons → triggers**: LMB/RMB/MMB routed via virtual keys
  `kLButton/kRButton/kMButton`.
- **Mouselook → right stick**: per-frame accumulated mouse delta is scaled by
  `mnk_sensitivity` × a base scale and written to `thumb_rx/thumb_ry`
  (`thumb_ry = -mouse_dy`, i.e. standard non-inverted). Deltas are reset each
  `GetState`.
- **Capture lifecycle**: `UpdateMouseCapture()` hides the cursor, captures the
  mouse, and re-centers it each frame while enabled+focused; releases on focus
  loss / window close; zeroes key state on focus loss.
- **Config + UI**: all binds and `mnk_mode` / `mnk_user_index` /
  `mnk_sensitivity` are cvars, persisted to `ge.toml` via
  `cvar::SaveConfig`, and exposed in the settings overlay with a live
  **Rebind** button (it even detects mouse-button rebinds). Controller binds are
  greyed out when `mnk_mode` is off.
- **Merge semantics**: `InputSystem::GetState` ORs buttons, takes max triggers
  and max-magnitude sticks across drivers — so KB/M and a gamepad can be used
  simultaneously without one zeroing the other.

This is a strong foundation. The remaining work is **gaps and polish**, not a
green-field build.

### 0.4 The gap — what KB/M + mouselook still needs

Ordered by severity.

1. **Mouselook is broken on Linux (headline gap).** The Linux window backend is
   **GTK** (`window_gtk.cpp`), not SDL. GTKWindow does **not** override
   `ApplyNewMouseCapture`, `ApplyNewMouseRelease`, or `ApplyNewCursorVisibility`
   — they fall through to the empty base implementations in `window.h`. And
   `MnkInputDriver::CenterCursor()` only warps the pointer under
   `#if REX_PLATFORM_WIN32` (via `SetCursorPos`). Net effect on Linux:
   the cursor is **not hidden, not grabbed, and not re-centered**, so the OS
   pointer drifts to the screen edge and the relative-delta computation
   (`dx = x − prev_x`) clamps to zero at the window border. Keyboard and mouse
   *buttons* work on Linux; **mouselook does not.** Windows works.

2. **Mouselook quality / fidelity.** Mouse motion is mapped to right-stick
   *deflection*, which the game interprets as a *turn rate* (velocity), not a
   1:1 angular displacement. This yields the familiar "mouse-as-stick" feel:
   continued deflection keeps turning, and aim is coupled to the game's
   internal stick→look curve and possibly an aim-acceleration ramp. It also has
   subtle frame-rate coupling (one accumulation window per `GetState`, scaled by
   a constant rather than by elapsed time). True 1:1 positional mouselook is
   **not reachable** without injecting into the game's own aim integrator — see
   §2.2 for options. v1 target is "good mouse-as-stick," matching Perfect Dark's
   own stated multiplayer caveat.

3. **No mouse-wheel binding (weapon cycling).** The keybind parser
   (`keybinds.cpp` `kKeyNames`) knows `LMB/RMB/MMB` but **not** wheel up/down or
   the side buttons. `MnkInputDriver` does not implement `OnMouseWheel`. Wheel
   weapon-cycling — a staple PC FPS binding and a first-class bind in Perfect
   Dark (`VK_MOUSE_WHEEL_UP/DN`) — is therefore impossible today.

4. **Limited mouse-button coverage.** `kXButton1/kXButton2` exist in
   `virtual_key.h` but are absent from the keybind parser and from the driver's
   `OnMouseDown/Up` switch (only L/R/M handled). Side buttons can't be bound.

5. **No invert-Y toggle and a single sensitivity scalar.** Invert is hardcoded
   (`-mouse_dy`); there is one `mnk_sensitivity` for both axes. PC players
   expect an invert-Y option and often separate X/Y sensitivity.

6. **No dead-zone-free guarantee for the emulated stick.** The game likely
   applies an inner dead zone to the right stick; small mouse movements below
   that threshold are dropped, hurting fine aim. Needs measurement and a
   dead-zone-compensation option (§2.2).

### 0.5 Reference: how Perfect Dark PC port does it (and why it differs)

Source: `perfect-dark-pc-port/perfect_dark` (branch `port`), files
`port/src/input.c`, `port/include/input.h`, `port/src/config.c`,
`port/include/config.h`.

- **SDL2 everywhere.** A single `inputEventFilter()` consumes
  `SDL_CONTROLLERDEVICEADDED`, `SDL_MOUSEWHEEL`, `SDL_KEYDOWN`, etc.; gamepad via
  `SDL_GameController`, keyboard via `SDL_GetKeyboardState`, mouse via
  `SDL_GetMouseState`. SDL relative-mouse mode gives clean deltas with no edge
  clamping, cross-platform.
- **Multi-bind action table.** `binds[MAXCONTROLLERS][CK_TOTAL_COUNT][INPUT_MAX_BINDS]`
  maps each *controller action* (`CK_A`, `CK_ZTRIG`, `CK_LTRIG`, …) to up to N
  physical inputs. Mouse buttons/wheel are first-class virtual keys
  (`VK_MOUSE_LEFT/RIGHT`, `VK_MOUSE_WHEEL_UP/DN`). Defaults match the issue
  (LMB fire, RMB aim, E/R/Q/F, etc.). Editable in `pd.ini` without recompiling.
- **Mouselook.** `mouseDX/mouseDY` scaled by `mouseSensX/mouseSensY`
  (`mdx = mouseDX * (0.022f/3.5f) * mouseSensX`) and exposed via
  `inputMouseGetScaledDelta()`.
- **Player-1 only for mouse** in practice (keyboard binds applied only when
  `cidx == 0`); mouse state is global.

**The decisive architectural difference.** Perfect Dark is a **decompilation**:
it has the game's source, so its mouse deltas are read by PD's *own* camera/aim
code and applied **directly to look angles** → true 1:1 mouselook, real
invert/sensitivity/accel, no stick emulation. GoldenEye Recomp is a **static
recompilation of a closed binary**: the only seam is `X_INPUT_GAMEPAD`, so mouse
motion must be *expressed as right-stick deflection*. PD is the right model for
*ergonomics* (binding table, SDL, wheel, sensitivity, P1-only) but **not**
reachable for *1:1 mouselook* without the deeper aim-injection work in §2.2.
This is the key finding to set expectations against.

---

## Phase 1 — Specification

### 1.1 Functional requirements

- **FR1 Movement.** Keyboard maps to the left stick (WASD default), full
  deflection, diagonals supported. *(Exists.)*
- **FR2 Actions.** All Xbox 360 face/shoulder/dpad/start/back buttons bindable to
  keyboard or mouse buttons. *(Exists; extend to side buttons — §0.4.4.)*
- **FR3 Fire/aim on mouse.** LMB → right trigger (fire), RMB → left trigger
  (aim). *(Exists.)*
- **FR4 Mouselook.** Mouse motion drives the right stick (look/aim) with
  adjustable sensitivity, on **both Windows and Linux**. *(Linux broken —
  §0.4.1.)*
- **FR5 Weapon cycling on wheel.** Mouse wheel up/down bindable (default: next/
  prev weapon). *(Missing — §0.4.3.)*
- **FR6 Invert-Y + sensitivity.** Invert-Y toggle; sensitivity scalar (stretch:
  separate X/Y). *(Invert missing.)*
- **FR7 Rebind UI + persistence.** All bindings rebindable in-game and persisted
  across launches. *(Exists via overlay + `ge.toml`.)*

### 1.2 Config / binding format

Reuse the **existing cvar + TOML system** (`ge.toml`, `cvar::Save/LoadConfig`).
Do **not** introduce a separate `pd.ini`-style file — the workspace already has a
working, UI-integrated, persisted config layer. New cvars needed:

| cvar | type | default | purpose |
| --- | --- | --- | --- |
| `mnk_invert_y` | bool | false | invert vertical mouselook |
| `mnk_sensitivity_y` | double | (= `mnk_sensitivity`) | optional separate Y sens (stretch) |
| `keybind_wheel_up` / `keybind_wheel_down` | string | weapon next/prev | wheel binds |
| `mnk_wheel_pulse_frames` | int | 2 | frames a wheel "tick" stays asserted |

Existing `mnk_mode`, `mnk_user_index`, `mnk_sensitivity`, `keybind_*` are kept.
Binding string grammar extends to accept `WheelUp`, `WheelDown`, `Mouse4`,
`Mouse5` (parser additions, §3.3).

### 1.3 Non-goals / constraints (v1)

- KB/M drives **one player only** (`mnk_user_index`, default P1) — matches PD.
- **Parity with gamepad behavior**, not beyond it: aim still flows through the
  right stick, so v1 does **not** promise frame-perfect 1:1 mouselook
  (see §2.2; that is an explicit stretch goal, not v1 acceptance).
- No new remap UI work required beyond extending the existing overlay to the new
  binds.
- **macOS out of scope** (this fork targets Windows/Linux/Android only).
- **Android out of scope** for KB/M (separate `AndroidInputDriver` path).

### 1.4 Acceptance criteria

- AC1: On **Windows and Linux**, with `mnk_mode=true`, the player can move, look
  (smooth mouselook with no edge-clamping/drift), fire, aim, switch weapons
  (wheel), and navigate menus using only keyboard + mouse.
- AC2: Rebinding any action (incl. mouse buttons and wheel), invert-Y, and
  sensitivity all take effect live and **persist** across a restart (`ge.toml`).
- AC3: A connected gamepad continues to work unchanged; KB/M and pad can be used
  in the same session without either zeroing the other.
- AC4: Releasing focus (Alt-Tab) releases mouse capture and restores the cursor;
  regaining focus re-captures cleanly with no aim spike.

---

## Phase 2 — Design

### 2.1 Input abstraction (already in place — keep)

The device-agnostic action layer already exists: cvar-named actions
(`keybind_*`) → `ParseVirtualKey` → `key_down_[]` test → `X_INPUT_GAMEPAD`
field, aggregated by `InputSystem::GetState`. No redesign. Extensions:
add wheel + side-button actions into the same scheme.

### 2.2 Mouselook design

**v1 — robust mouse-as-stick (cross-platform).** Keep the
delta→`thumb_rx/ry` mapping but make it correct and tunable:

1. **Cross-platform relative mouse.** Replace the platform-specific re-center
   with reliable relative-motion capture (§2.4). Accumulate raw deltas in
   `OnMouseMove`; reset per `GetState`.
2. **Frame-rate independence.** Scale by elapsed time (or, since the game samples
   the pad once per frame, normalize the accumulated delta to a per-poll basis)
   so turn feel is stable across 30/60/uncapped.
3. **Invert + sensitivity.** Apply `mnk_invert_y` and (optionally)
   `mnk_sensitivity_y`. `thumb_ry = (invert ? +dy : -dy) * sens_y`.
4. **Dead-zone compensation.** Measure GoldenEye's right-stick inner dead zone
   empirically; bias small non-zero deltas up to just past the dead zone so fine
   aim is not swallowed. Expose as `mnk_deadzone_comp` if measurement shows it's
   needed.
5. **Saturation curve.** Clamp to `INT16` (exists); consider a mild response
   curve so fast flicks don't instantly peg max turn rate.

**Stretch — true 1:1 mouselook via aim injection.** The only way to get
positional 1:1 mouselook is to bypass the stick and write into the game's aim
state directly. On a static recomp this means: locate the function/variable that
integrates right-stick input into the player's yaw/pitch (via the recompiler's
`ge_config.toml` function map + a **mid-asm hook**, the same mechanism GE Recomp
already uses for other fixups — see `[[midasm_hook]]` entries in
`ge_config.toml`), and add the scaled mouse delta to the look angle there each
frame, while suppressing the synthetic right-stick contribution. This is a
research spike with meaningful reverse-engineering cost; it is explicitly a
**post-v1** goal and gated on confirming the aim-integration site. v1 ships the
mouse-as-stick path.

### 2.3 Config layer

No new system. New cvars (§1.2) registered with `REXCVAR_DEFINE_*` in the same
`"Input"` / `"Input/Keybinds/Controller"` categories so they auto-appear in the
settings overlay and persist via `SaveConfig` to `ge.toml` (next to the
executable; resolved in `rex_app.cpp` `config_path_`).

### 2.4 Cross-platform capture (the core fix)

Implement true relative-mouse capture on each backend behind the existing
`Window::CaptureMouse/ReleaseMouse/SetCursorVisibility` API so the MnK driver
stays platform-agnostic:

- **Windows (works; harden):** already grabs (`SetCapture`) and warps
  (`SetCursorPos`). Optionally migrate to `RAWINPUT` (`WM_INPUT`) for true
  high-resolution relative deltas independent of pointer ballistics/clamping.
- **Linux/GTK (the fix):** override `GTKWindow::ApplyNewMouseCapture`,
  `ApplyNewMouseRelease`, and `ApplyNewCursorVisibility` to (a) hide the cursor
  (blank `GdkCursor`), (b) grab the pointer via the GDK seat
  (`gdk_seat_grab`), and (c) provide relative motion — either by GDK pointer
  warp + re-center each frame (mirroring the Win32 path; move the warp out of
  the `#if REX_PLATFORM_WIN32` block in `CenterCursor` into a virtual
  `Window::WarpCursorToCenter()`), or preferably by enabling raw/relative motion
  events. Handle focus-in/out to grab/ungrab.
- **Driver change:** replace the `#if REX_PLATFORM_WIN32` warp in
  `MnkInputDriver::CenterCursor()` with a call to a new virtual
  `Window::WarpCursorToCenter()` (default no-op; Win32 + GTK override it). This
  removes all platform `#if`s from the driver.

### 2.5 Cross-platform considerations

- **Scancode vs virtual key.** GTK and Win32 already normalize into
  `rex::ui::VirtualKey` before reaching the driver; keep using that — do not
  introduce SDL scancodes (windowing here is GTK/Win32, not SDL).
- **Focus loss.** Already handled (`OnLostFocus` zeroes keys, releases capture);
  verify GTK delivers focus events to `WindowListener`.
- **Wheel events.** GTK already produces scroll events (`HandleMouse` →
  `OnMouseWheel`) and Win32 has `WM_MOUSEWHEEL`; route both into a new
  `MnkInputDriver::OnMouseWheel` (§3.3).

---

## Phase 3 — Implementation plan

1. **Cross-platform capture (unblocks Linux mouselook).**
   - Add virtual `Window::WarpCursorToCenter()`; implement in `window_win.cpp`
     (move existing logic) and `window_gtk.cpp`.
   - Implement `GTKWindow::ApplyNewMouseCapture/Release/CursorVisibility`
     (hide cursor, seat grab/ungrab).
   - Refactor `MnkInputDriver::CenterCursor()` to call the virtual; delete the
     `#if REX_PLATFORM_WIN32`.
2. **Mouselook polish.** Frame-rate-independent scaling; `mnk_invert_y`
   (+ optional `mnk_sensitivity_y`); optional dead-zone compensation cvar.
3. **Mouse wheel + side buttons.**
   - Extend `keybinds.cpp` `kKeyNames` with `WheelUp/WheelDown/Mouse4/Mouse5`
     and matching `VirtualKeyToString`.
   - Add `MnkInputDriver::OnMouseWheel` (override the listener hook): translate a
     wheel tick into a short pulse of the bound action (held for
     `mnk_wheel_pulse_frames` so the game registers a discrete press).
   - Handle `kXButton1/2` in `OnMouseDown/Up`.
   - Register `keybind_wheel_up/down` cvars with weapon-cycle defaults.
4. **Keep gamepad intact.** No change to merge logic; verify simultaneous use.

### 3.5 Effort estimate

| Task | Approx. |
| --- | --- |
| GTK capture + warp + cross-platform refactor | M (the bulk; mostly GTK/GDK) |
| Mouselook polish (invert, frame-rate, deadzone) | S |
| Wheel + side-button binds | S–M |
| True 1:1 aim-injection (stretch) | L + RE spike (post-v1) |

---

## Phase 4 — Verification

1. **Functional, both OSes.** On Windows and Linux: move, look (no
   drift/edge-clamp), fire, aim, switch weapons via wheel, navigate menus —
   keyboard+mouse only.
2. **Config.** Rebind each action incl. mouse buttons + wheel; toggle invert-Y;
   change sensitivity. Confirm live effect and persistence across restart
   (inspect `ge.toml`).
3. **No gamepad regression.** Plug in an Xbox pad: confirm unchanged behavior and
   simultaneous KB/M + pad use.
4. **Focus handling.** Alt-Tab away and back; confirm cursor restore and clean
   re-capture with no aim spike.
5. **CI.** Compile-verify on Linux (libc++) and Windows (MSVC) via the existing
   `.github/workflows/build.yml`.

---

## Summary of findings

- GoldenEye Recomp is an **Xbox 360/XBLA static recomp on ReXGlue** (Xenia
  lineage), so input is the **XInput `X_INPUT_GAMEPAD`** contract — *not* N64.
- A capable **MnK driver already exists and ships** (rebindable cvar binds,
  mouse-as-stick mouselook, capture lifecycle, settings-overlay rebind UI, TOML
  persistence). This is polish-and-fix work, not green-field.
- **Top gap: Linux mouselook is broken** because the GTK window backend never
  implements mouse capture / cursor-hide / pointer-warp, and the driver's
  re-center is Win32-only.
- Secondary gaps: no wheel/side-button binds, no invert-Y, single sensitivity,
  frame-rate-coupled scaling, no dead-zone compensation.
- **Perfect Dark** (SDL2, multi-bind table, wheel as first-class, P1-only mouse)
  is the right ergonomics reference, but it gets **true 1:1 mouselook only
  because it's a decompilation** writing into the game's own aim code. On this
  static recomp, v1 is "robust mouse-as-stick"; true 1:1 mouselook is a post-v1
  aim-injection spike via the existing mid-asm hook mechanism.
