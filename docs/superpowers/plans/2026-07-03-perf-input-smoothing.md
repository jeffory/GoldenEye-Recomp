# Framerate Stability + Linux Mouse Smoothing Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement the framerate-stability and Linux mouse-smoothing fixes from the 2026-07-03 investigation: unpin the present path from the UI thread by making passive overlays conditional, boost the guest-clock (VSync) worker priority, and replace warp-based mouse deltas with XInput2 raw relative motion plus float precision and optional smoothing.

**Architecture:** Two repos change together. The **ReXGlue SDK** (`/home/keith/Projects/GoldenEye-Recomp-rexglue`, linked as a subproject — rebuilding the game target `ge` rebuilds it) gains a raw-relative-motion event path (`MouseRelativeEvent` → `WindowInputListener::OnMouseRelativeMotion` → XI2 raw events in `GTKWindow`) and a priority boost for the `GPU VSync` worker. The **game** (`/home/keith/Projects/GoldenEye-Recomp`) consumes the new event in `GameInputListener` (float accumulation, ignores accelerated position-diff deltas while raw flows), adds a `ge_mouse_smooth` EMA cvar, and manages overlay-dialog lifetime so the ImGui drawer only registers with the presenter while an overlay actually draws (an registered UI drawer forces `PaintMode::kUIThreadOnRequest` — see `presenter.cpp:1390-1394`).

**Tech Stack:** C++17, CMake presets, GTK3/GDK + X11/XInput2 (Linux windowing), ImGui (overlays/menu), REXCVAR cvar system.

## Global Constraints

- Branch name in BOTH repos: `feat/perf-input-smoothing` (game branches off `develop`, SDK off `main`).
- Build command (builds SDK + game together): `cd /home/keith/Projects/GoldenEye-Recomp && cmake --build --preset linux-amd64-relwithdebinfo --target ge` — every task must end with this compiling cleanly.
- There is NO unit-test infrastructure for windowing/guest-RAM code in either repo. "Test" for Tasks 2–7 = clean compile; Task 8 is the consolidated runtime verification (run the game, check logs + feel).
- Do not touch `ge_dbg_now` / the 80ms skip-bit fallback (`ge_hooks.cpp:647-804`) — it is freeze protection, recently fixed, verified on-device.
- Do not change Android behavior except the one line specified in Task 5 (`ge_fps_overlay` no longer forced on).
- SDK code style: Xenia-derived (2-space indent, `destruction_receiver` pattern for anything that dispatches listener events). Game code style: heavily commented, comments explain *why*.
- Commit after each task, in the repo the task touched. No pushing.

---

### Task 1: Create branches in both repos

**Files:** none (git only)

- [ ] **Step 1: Verify both repos are clean and create branches**

```bash
cd /home/keith/Projects/GoldenEye-Recomp && git status --short && git checkout -b feat/perf-input-smoothing develop
cd /home/keith/Projects/GoldenEye-Recomp-rexglue && git status --short && git checkout -b feat/perf-input-smoothing main
```

Expected: no uncommitted changes in either repo; both commands end on the new branch.

---

### Task 2: Boost the GPU VSync worker thread priority (SDK)

The `GPU VSync` worker is the guest's only vblank/clock source. It was created with `creation_flags=0` (normal priority) while the CP worker was recently boosted to `0x20` (kAboveNormal) because yield-spinning guest threads starve workers on few-core devices — the same hazard applies to the clock source: if it is descheduled, the entire guest logic stutters.

**Files:**
- Modify: `/home/keith/Projects/GoldenEye-Recomp-rexglue/src/graphics/graphics_system.cpp:161-162`

**Interfaces:**
- Consumes: `system::XHostThread(kernel_state, stack, creation_flags, fn)` — flags `& 0x60` map to host priority even when `ignore_thread_priorities` is set (see `xthread.cpp:463-464`); `0x20` = kAboveNormal (same value the CP worker uses at `command_processor.cpp:219`).
- Produces: nothing new — behavior change only.

- [ ] **Step 1: Apply the edit**

In `graphics_system.cpp`, change:

```cpp
  vsync_worker_thread_ = system::object_ref<system::XHostThread>(
      new system::XHostThread(kernel_state_, 128 * 1024, 0, [this]() {
```

to:

```cpp
  // kAboveNormal (creation_flags 0x20), same boost as the CP worker
  // (command_processor.cpp): this thread is the guest's only vblank/clock
  // source (MarkVblank below drives the vblank ISR). On few-core handhelds
  // yield-spinning guest threads can starve normal-priority workers; a
  // descheduled clock stutters ALL guest logic, so it must win the core.
  vsync_worker_thread_ = system::object_ref<system::XHostThread>(
      new system::XHostThread(kernel_state_, 128 * 1024, /*creation_flags=*/0x20, [this]() {
```

- [ ] **Step 2: Build**

Run: `cd /home/keith/Projects/GoldenEye-Recomp && cmake --build --preset linux-amd64-relwithdebinfo --target ge`
Expected: clean build (librexruntimerd.so relinks).

- [ ] **Step 3: Commit (SDK repo)**

```bash
cd /home/keith/Projects/GoldenEye-Recomp-rexglue
git add src/graphics/graphics_system.cpp
git commit -m "perf(gpu): boost the GPU VSync worker to kAboveNormal

The vsync worker is the guest's only vblank/clock source. Give it the
same 0x20 creation-flags priority boost as the CP worker so yield-
spinning guest threads can't starve the guest clock on few-core
devices (a descheduled clock stutters all guest logic)."
```

---

### Task 3: Add the relative-motion event + listener + Window dispatch (SDK)

New event type carrying float, unaccelerated pointer deltas, a listener virtual, and the `Window` dispatch helper mirroring `OnMouseMove`.

**Files:**
- Modify: `/home/keith/Projects/GoldenEye-Recomp-rexglue/include/rex/ui/ui_event.h` (after `MouseEvent`, ~line 150)
- Modify: `/home/keith/Projects/GoldenEye-Recomp-rexglue/include/rex/ui/window_listener.h` (in `WindowInputListener`, after `OnMouseWheel`, line 50)
- Modify: `/home/keith/Projects/GoldenEye-Recomp-rexglue/include/rex/ui/window.h` (protected dispatchers, after line 595 `OnMouseWheel`)
- Modify: `/home/keith/Projects/GoldenEye-Recomp-rexglue/src/ui/window.cpp` (after `Window::OnMouseMove`, ~line 651)

**Interfaces:**
- Produces: `class rex::ui::MouseRelativeEvent : public UIEvent` with `float dx() const`, `float dy() const`, `bool is_handled()`, `void set_handled(bool)`; ctor `MouseRelativeEvent(Window* target, float dx, float dy)`.
- Produces: `virtual void WindowInputListener::OnMouseRelativeMotion(MouseRelativeEvent&) {}` (default no-op — existing listeners unaffected).
- Produces: `void Window::OnMouseRelativeMotion(MouseRelativeEvent& e, WindowDestructionReceiver& destruction_receiver)` (protected; used by Task 4, overridden game-side in Task 6).

- [ ] **Step 1: Add `MouseRelativeEvent` to `ui_event.h`**

Insert directly after the closing `};` of `class MouseEvent` (before `class TouchEvent`):

```cpp
// Relative pointer motion (e.g. X11 XInput2 raw events) with sub-pixel
// precision and WITHOUT OS pointer acceleration. dx/dy are unaccelerated
// pixels-ish device units; positive x is right, positive y is down. Only
// delivered while a platform raw-motion source is active (mouse captured on
// X11) - consumers should ignore deltas synthesized from absolute
// OnMouseMove positions while these flow.
class MouseRelativeEvent : public UIEvent {
 public:
  explicit MouseRelativeEvent(Window* target, float dx, float dy)
      : UIEvent(target), dx_(dx), dy_(dy) {}
  ~MouseRelativeEvent() override = default;

  bool is_handled() const { return handled_; }
  void set_handled(bool value) { handled_ = value; }

  float dx() const { return dx_; }
  float dy() const { return dy_; }

 private:
  bool handled_ = false;
  float dx_ = 0.0f;
  float dy_ = 0.0f;
};
```

- [ ] **Step 2: Add the listener virtual to `window_listener.h`**

In `class WindowInputListener`, after `virtual void OnMouseWheel(MouseEvent&) {}`:

```cpp
  // Raw relative pointer motion (no OS acceleration; see MouseRelativeEvent).
  virtual void OnMouseRelativeMotion(MouseRelativeEvent&) {}
```

- [ ] **Step 3: Declare the dispatcher in `window.h`**

After `void OnMouseWheel(MouseEvent& e, WindowDestructionReceiver& destruction_receiver);` (line 595):

```cpp
  void OnMouseRelativeMotion(MouseRelativeEvent& e,
                             WindowDestructionReceiver& destruction_receiver);
```

- [ ] **Step 4: Implement the dispatcher in `window.cpp`**

After the `Window::OnMouseMove` implementation (mirrors it exactly):

```cpp
void Window::OnMouseRelativeMotion(MouseRelativeEvent& e,
                                   WindowDestructionReceiver& destruction_receiver) {
  PropagateEventThroughInputListeners(
      [&e](auto listener) {
        listener->OnMouseRelativeMotion(e);
        return e.is_handled();
      },
      destruction_receiver);
  if (destruction_receiver.IsWindowDestroyed()) {
    return;
  }
}
```

- [ ] **Step 5: Build**

Run: `cd /home/keith/Projects/GoldenEye-Recomp && cmake --build --preset linux-amd64-relwithdebinfo --target ge`
Expected: clean build.

- [ ] **Step 6: Commit (SDK repo)**

```bash
cd /home/keith/Projects/GoldenEye-Recomp-rexglue
git add include/rex/ui/ui_event.h include/rex/ui/window_listener.h include/rex/ui/window.h src/ui/window.cpp
git commit -m "feat(ui): add MouseRelativeEvent + OnMouseRelativeMotion listener path

New event type for raw, unaccelerated, sub-pixel pointer deltas
(float dx/dy), a default-no-op WindowInputListener virtual, and the
Window dispatch helper mirroring OnMouseMove. No platform delivers it
yet - the GTK/XI2 source lands next."
```

---

### Task 4: Deliver XI2 raw motion from GTKWindow (SDK)

While the pointer is captured (mouse-look), select `XI_RawMotion` on the X11 root window and deliver unaccelerated deltas through the new event path. This bypasses desktop pointer acceleration, integer-pixel truncation, and the warp echo-cancel race in one change. Wayland/non-X11 keeps the existing warp/position-difference fallback (the game runs under XWayland anyway — `CreateSurfaceImpl` is X11-only).

**Files:**
- Modify: `/home/keith/Projects/GoldenEye-Recomp-rexglue/include/rex/ui/window_gtk.h` (private section of `GTKWindow`)
- Modify: `/home/keith/Projects/GoldenEye-Recomp-rexglue/src/ui/window_gtk.cpp` (includes; `ApplyNewMouseCapture` line ~579; `ApplyNewMouseRelease` line ~597; `WindowEventHandler` GDK_DELETE case line ~821; new methods near the other Apply* impls; destructor)
- Modify: `/home/keith/Projects/GoldenEye-Recomp-rexglue/src/ui/CMakeLists.txt` (lines 160-171, Linux branch)

**Interfaces:**
- Consumes: `MouseRelativeEvent`, `Window::OnMouseRelativeMotion(e, receiver)` from Task 3; `WindowDestructionReceiver destruction_receiver(this)` pattern (as used in `WindowEventHandler`).
- Produces: raw-motion delivery on X11; log line `"GTKWindow: XI2 raw mouse motion enabled"` (Task 8 verification hook).

- [ ] **Step 1: Add members/methods to `window_gtk.h`**

In `GTKWindow`'s private section, after `bool pointer_grabbed_ = false;`:

```cpp
  // X11 XInput2 raw relative motion: the mouse-look delta source while the
  // pointer is captured. Bypasses OS pointer acceleration and gives float
  // sub-pixel deltas (see MouseRelativeEvent). Selected on the root window
  // for XIAllMasterDevices; no-op on non-X11 backends (warp fallback stays).
  void EnableRawMotion();
  void DisableRawMotion();
  static GdkFilterReturn RawEventFilterThunk(GdkXEvent* xevent, GdkEvent* event, gpointer data);
  GdkFilterReturn RawEventFilter(GdkXEvent* xevent);
  int xi2_opcode_ = -1;
  bool raw_motion_selected_ = false;
```

- [ ] **Step 2: Add the XInput2 include to `window_gtk.cpp`**

After `#include <X11/Xlib-xcb.h>` (line 26):

```cpp
#include <X11/extensions/XInput2.h>
```

- [ ] **Step 3: Implement enable/disable/filter in `window_gtk.cpp`**

Insert after `GTKWindow::ApplyWarpMouseToClient` (line ~637):

```cpp
void GTKWindow::EnableRawMotion() {
  if (raw_motion_selected_ || !window_) {
    return;
  }
  GdkDisplay* display = gtk_widget_get_display(window_);
  if (!GDK_IS_X11_DISPLAY(display)) {
    return;  // Wayland etc.: the warp/position-difference fallback stays.
  }
  Display* xdisplay = gdk_x11_display_get_xdisplay(display);
  if (xi2_opcode_ < 0) {
    int event_base, error_base;
    if (!XQueryExtension(xdisplay, "XInputExtension", &xi2_opcode_, &event_base, &error_base)) {
      xi2_opcode_ = -1;
      return;
    }
    int major = 2, minor = 0;
    if (XIQueryVersion(xdisplay, &major, &minor) != Success) {
      xi2_opcode_ = -1;
      return;
    }
  }
  unsigned char mask_bits[XIMaskLen(XI_LASTEVENT)] = {};
  XISetMask(mask_bits, XI_RawMotion);
  XIEventMask mask;
  mask.deviceid = XIAllMasterDevices;
  mask.mask_len = sizeof(mask_bits);
  mask.mask = mask_bits;
  XISelectEvents(xdisplay, DefaultRootWindow(xdisplay), &mask, 1);
  XFlush(xdisplay);
  gdk_window_add_filter(nullptr, RawEventFilterThunk, this);
  raw_motion_selected_ = true;
  REXLOG_INFO("GTKWindow: XI2 raw mouse motion enabled");
}

void GTKWindow::DisableRawMotion() {
  if (!raw_motion_selected_) {
    return;
  }
  gdk_window_remove_filter(nullptr, RawEventFilterThunk, this);
  raw_motion_selected_ = false;
  if (!window_) {
    return;  // Window already torn down; the X connection selection dies with it.
  }
  GdkDisplay* display = gtk_widget_get_display(window_);
  if (!GDK_IS_X11_DISPLAY(display)) {
    return;
  }
  Display* xdisplay = gdk_x11_display_get_xdisplay(display);
  unsigned char mask_bits[XIMaskLen(XI_LASTEVENT)] = {};  // empty = deselect
  XIEventMask mask;
  mask.deviceid = XIAllMasterDevices;
  mask.mask_len = sizeof(mask_bits);
  mask.mask = mask_bits;
  XISelectEvents(xdisplay, DefaultRootWindow(xdisplay), &mask, 1);
  XFlush(xdisplay);
}

GdkFilterReturn GTKWindow::RawEventFilterThunk(GdkXEvent* xevent, GdkEvent* /*event*/,
                                               gpointer data) {
  return reinterpret_cast<GTKWindow*>(data)->RawEventFilter(xevent);
}

GdkFilterReturn GTKWindow::RawEventFilter(GdkXEvent* xevent) {
  XEvent* xev = reinterpret_cast<XEvent*>(xevent);
  if (!window_ || xi2_opcode_ < 0 || xev->type != GenericEvent ||
      xev->xcookie.extension != xi2_opcode_ || xev->xcookie.evtype != XI_RawMotion) {
    return GDK_FILTER_CONTINUE;
  }
  Display* xdisplay = gdk_x11_display_get_xdisplay(gtk_widget_get_display(window_));
  // GDK usually fetches the cookie data before running filters; fetch (and
  // free) it ourselves only if it hasn't.
  XGenericEventCookie* cookie = &xev->xcookie;
  bool own_cookie_data = false;
  if (!cookie->data) {
    if (!XGetEventData(xdisplay, cookie)) {
      return GDK_FILTER_CONTINUE;
    }
    own_cookie_data = true;
  }
  const XIRawEvent* raw = static_cast<const XIRawEvent*>(cookie->data);
  // Valuators 0/1 are pointer X/Y. raw_values are the UNaccelerated deltas
  // (event->valuators would be post-acceleration).
  double dx = 0.0, dy = 0.0;
  const double* value = raw->raw_values;
  if (raw->valuators.mask_len > 0) {
    if (XIMaskIsSet(raw->valuators.mask, 0)) dx = *value++;
    if (raw->valuators.mask_len * 8 > 1 && XIMaskIsSet(raw->valuators.mask, 1)) dy = *value++;
  }
  if (own_cookie_data) {
    XFreeEventData(xdisplay, cookie);
  }
  if (dx != 0.0 || dy != 0.0) {
    MouseRelativeEvent e(this, float(dx), float(dy));
    WindowDestructionReceiver destruction_receiver(this);
    OnMouseRelativeMotion(e, destruction_receiver);
    if (destruction_receiver.IsWindowDestroyed()) {
      return GDK_FILTER_REMOVE;
    }
  }
  // Consumed: raw events were selected by us, GDK has no use for them.
  return GDK_FILTER_REMOVE;
}
```

- [ ] **Step 4: Hook capture/release/teardown**

In `ApplyNewMouseCapture`, after `pointer_grabbed_ = true;`:

```cpp
    pointer_grabbed_ = true;
    // Raw XI2 deltas drive mouse-look while captured (no OS acceleration).
    EnableRawMotion();
```

In `ApplyNewMouseRelease`, before `gdk_seat_ungrab(seat);`:

```cpp
  DisableRawMotion();
```

In `WindowEventHandler`'s `GDK_DELETE`/`GDK_DESTROY` case, immediately before `GtkWidget* window = window_;`:

```cpp
      DisableRawMotion();
```

In `~GTKWindow` (find the destructor; if it exists add at its top, if not rely on the close path — check first), ensure the filter can't outlive the object:

```cpp
  DisableRawMotion();
```

- [ ] **Step 5: Link libXi in `src/ui/CMakeLists.txt`**

In the Linux `else()` branch (lines 160-171), add the `xi` module:

```cmake
    pkg_check_modules(GTK3 REQUIRED gtk+-3.0)
    pkg_check_modules(X11_XCB REQUIRED x11-xcb)
    pkg_check_modules(XI REQUIRED xi)

    target_include_directories(rexui PRIVATE
        ${GTK3_INCLUDE_DIRS}
        ${X11_XCB_INCLUDE_DIRS}
        ${XI_INCLUDE_DIRS}
    )
    target_link_libraries(rexui PUBLIC
        ${GTK3_LIBRARIES}
        ${X11_XCB_LIBRARIES}
        ${XI_LIBRARIES}
    )
```

- [ ] **Step 6: Build**

Run: `cd /home/keith/Projects/GoldenEye-Recomp && cmake --build --preset linux-amd64-relwithdebinfo --target ge`
Expected: clean build. If `pkg_check_modules(XI ...)` fails, `libXi-devel` needs installing (`sudo dnf install libXi-devel`) — surface this to the user rather than working around it.

- [ ] **Step 7: Commit (SDK repo)**

```bash
cd /home/keith/Projects/GoldenEye-Recomp-rexglue
git add include/rex/ui/window_gtk.h src/ui/window_gtk.cpp src/ui/CMakeLists.txt
git commit -m "feat(ui): XI2 raw mouse motion while captured on X11

While the pointer is captured for mouse-look, select XI_RawMotion on
the root window and deliver unaccelerated float deltas through the new
MouseRelativeEvent path. Removes desktop pointer acceleration, integer
pixel truncation, and the per-frame warp echo-cancel race from look
input in one change. Non-X11 backends keep the warp fallback."
```

---

### Task 5: Overlay dialog lifecycle — unpin the fast present path (game)

An `ImGuiDialog`'s constructor/destructor is what adds/removes it from the drawer (`imgui_dialog.cpp`), and the drawer registers with the presenter only while it has ≥1 dialog (`imgui_drawer.cpp:65-75, 92`). Any registered UI drawer forces every present onto the UI thread (`presenter.cpp:1390-1394`). Today `PostFxOverlay` and `FpsOverlay` are constructed once at startup and live forever, even when their cvars make `OnDraw` a no-op — so the low-latency `kGuestOutputThreadImmediately` path is structurally unreachable. Fix: create/destroy the overlay objects to match their cvars.

**Files:**
- Modify: `/home/keith/Projects/GoldenEye-Recomp/src/ge_app.h` (OnCreateDialogs ~line 110-138, Android defaults ~line 77, TogglePauseMenu ~line 159-215, members ~line 217-221)
- Modify: `/home/keith/Projects/GoldenEye-Recomp/src/ge_menu.h` (`Callbacks` struct, line 25-45)
- Modify: `/home/keith/Projects/GoldenEye-Recomp/src/ge_menu.cpp` (FPS Overlay checkbox ~line 641, Post-FX checkbox ~line 563, preset combo ~line 571)

**Interfaces:**
- Consumes: `rex::cvar::GetFlagByName(name) == "true"` (the by-name bool read used by `ge_menu.cpp:32`); `app_context().CallInUIThreadDeferred(fn)` (same deferral as `request_fullscreen` — overlay create/destroy must NOT run inside the menu's own ImGui draw).
- Produces: `GeApp::UpdateOverlayRegistration()` (private), `GeMenuDialog::Callbacks::overlays_changed` (`std::function<void()>`), called by the menu after any change to `ge_fps_overlay` / `postfx_enabled` / a preset apply.

- [ ] **Step 1: Add the callback field to `ge_menu.h`**

In `struct Callbacks`, after `set_perf_csv`:

```cpp
    // Invoked (deferred to the UI loop by the app) after the menu changes
    // ge_fps_overlay / postfx_enabled / applies a preset, so the app can
    // create/destroy the overlay dialogs to match. Overlay dialogs are only
    // kept alive while they draw: a registered (even invisible) UI drawer
    // pins every present to the UI thread (Presenter paint-mode selection),
    // killing the low-latency guest-thread present path.
    std::function<void()> overlays_changed;
```

- [ ] **Step 2: Rework overlay creation in `ge_app.h`**

Replace the two unconditional creations in `OnCreateDialogs`:

```cpp
    postfx_ = std::make_unique<ge::PostFxOverlay>(drawer);
    fps_overlay_ = std::make_unique<ge::FpsOverlay>(drawer);  // guest-FPS readout
```

with:

```cpp
    drawer_ = drawer;
    UpdateOverlayRegistration();  // overlays exist only while their cvar is on
```

Add the private method (above `TogglePauseMenu`) and the `drawer_` member (next to `postfx_`):

```cpp
  // Create/destroy the passive overlays to match their cvars. An ImGuiDialog's
  // existence is what registers the ImGui drawer with the presenter, and ANY
  // registered UI drawer forces presents onto the UI thread
  // (Presenter::GetDesiredPaintModeFromUIThread) -- an always-on but invisible
  // overlay silently disables the low-latency guest-thread present path. Only
  // safe to call from the UI loop between frames (use CallInUIThreadDeferred
  // from menu callbacks -- the menu runs inside the drawer's own draw).
  void UpdateOverlayRegistration() {
    if (!drawer_) return;
    const bool want_postfx = rex::cvar::GetFlagByName("postfx_enabled") == "true";
    const bool want_fps = rex::cvar::GetFlagByName("ge_fps_overlay") == "true";
    if (want_postfx && !postfx_) postfx_ = std::make_unique<ge::PostFxOverlay>(drawer_);
    if (!want_postfx && postfx_) postfx_.reset();
    if (want_fps && !fps_overlay_) fps_overlay_ = std::make_unique<ge::FpsOverlay>(drawer_);
    if (!want_fps && fps_overlay_) fps_overlay_.reset();
  }
```

```cpp
  rex::ui::ImGuiDrawer* drawer_ = nullptr;          // set once in OnCreateDialogs
```

In `TogglePauseMenu`, wire the callback (next to `cb.persist_config`):

```cpp
    cb.overlays_changed = [this] {
      // Deferred: the menu invokes this from inside the drawer's draw, and
      // creating/destroying dialogs mid-draw is the same hazard as the
      // fullscreen switch above.
      app_context().CallInUIThreadDeferred([this] { UpdateOverlayRegistration(); });
    };
```

- [ ] **Step 3: Stop forcing the FPS overlay on Android**

In `OnConfigurePaths`, replace:

```cpp
    rex::cvar::SetFlagByName("ge_fps_overlay", "true");
    rex::cvar::SetFlagByName("ge_fps_log", "true");
```

with:

```cpp
    // GEFPS logging stays on (it needs no UI drawer), but the on-screen
    // overlay now defaults OFF: a registered overlay dialog pins every
    // present to the UI thread (see UpdateOverlayRegistration), which on the
    // handheld quantizes the shown rate down (GESHOWN "22 shown / 52
    // produced"). Toggle it per-session from the pause menu VIDEO tab.
    rex::cvar::SetFlagByName("ge_fps_log", "true");
```

- [ ] **Step 4: Notify from the menu in `ge_menu.cpp`**

FPS Overlay checkbox (~line 641) — add the callback call:

```cpp
      if (ImGui::Checkbox("FPS Overlay", &fps_overlay)) {
        SetCvarB("ge_fps_overlay", fps_overlay);
        if (callbacks_.persist_config) callbacks_.persist_config();
        if (callbacks_.overlays_changed) callbacks_.overlays_changed();
      }
```

Post-FX checkbox (~line 563) — same addition:

```cpp
      if (ImGui::Checkbox("Enable Post-FX", &pfx_on)) {
        SetCvarB("postfx_enabled", pfx_on);
        if (callbacks_.persist_config) callbacks_.persist_config();
        if (callbacks_.overlays_changed) callbacks_.overlays_changed();
      }
```

Preset combo (~line 572) — presets flip `postfx_enabled`:

```cpp
          if (ImGui::Selectable(ge::PostFxPresetName(i))) {
            ge::ApplyPostFxPreset(i);
            if (callbacks_.overlays_changed) callbacks_.overlays_changed();
          }
```

Also check the "Reset to defaults" button that calls `ge::ResetPostFx()` (~line 630) and add the same `overlays_changed` call after it.

- [ ] **Step 5: Build**

Run: `cd /home/keith/Projects/GoldenEye-Recomp && cmake --build --preset linux-amd64-relwithdebinfo --target ge`
Expected: clean build.

- [ ] **Step 6: Commit (game repo)**

```bash
cd /home/keith/Projects/GoldenEye-Recomp
git add src/ge_app.h src/ge_menu.h src/ge_menu.cpp
git commit -m "perf(present): only register overlay dialogs while they draw

An ImGuiDialog's existence registers the drawer with the presenter,
and any registered UI drawer forces presents onto the UI thread
(Presenter::GetDesiredPaintModeFromUIThread) - the always-alive
PostFx/FPS overlays were structurally disabling the low-latency
guest-thread present path even when invisible. Create/destroy them to
match their cvars (deferred to the UI loop from menu callbacks), and
stop forcing the FPS overlay on for Android (GEFPS logging stays on;
the overlay is a per-session pause-menu toggle now)."
```

---

### Task 6: Float mouse deltas + raw-motion consumption (game)

`GameInputListener` accumulates int pixel deltas by differencing warped positions. Switch accumulation to float, consume the new raw events, and ignore position-diff deltas while raw motion flows.

**Files:**
- Modify: `/home/keith/Projects/GoldenEye-Recomp/src/ge_hooks.cpp` (GameInputListener ~lines 1020-1118; `ge_take_mouse_dx/dy` lines 1138-1139; `ge_mouse_camera` lines 1202-1203)

**Interfaces:**
- Consumes: `rex::ui::MouseRelativeEvent` / `OnMouseRelativeMotion` from Task 3.
- Produces: `float ge_take_mouse_dx()`, `float ge_take_mouse_dy()` (were `int`) — `ge_mouse_camera` is the only caller (verified: `ge_hooks.cpp:1202-1203`).

- [ ] **Step 1: Convert accumulators to float and consume raw motion**

In `GameInputListener` replace the members `int dx_ = 0, dy_ = 0;` with:

```cpp
  float dx_ = 0.f, dy_ = 0.f;
  bool raw_motion_ = false;  // XI2 raw deltas flowing -> position diffs are noise
```

Replace `take_dx`/`take_dy` (lines 1021-1022):

```cpp
  // Per-frame consumption by the look hook (reset-on-read). Float: raw XI2
  // deltas are sub-pixel, and slow precise aim lives in the fractions.
  float take_dx() { std::lock_guard<std::mutex> l(m_); float v = dx_; dx_ = 0.f; return v; }
  float take_dy() { std::lock_guard<std::mutex> l(m_); float v = dy_; dy_ = 0.f; return v; }
```

Update the zeroings at `set_suppressed` (line 1036), capture engage (line 1052), and `OnLostFocus` (line 1085) from `dx_ = 0; dy_ = 0;` to `dx_ = 0.f; dy_ = 0.f;`. At the capture-engage site (inside `tick_capture`) also reset the raw flag so the warp fallback re-arms if raw ever stops:

```cpp
      std::lock_guard<std::mutex> l(m_);  // no spike on capture start
      dx_ = 0.f; dy_ = 0.f; have_prev_ = false; raw_motion_ = false;
```

Replace `OnMouseMove` (lines 1069-1074) and add the raw override after it:

```cpp
  void OnMouseMove(rex::ui::MouseEvent& e) override {
    std::lock_guard<std::mutex> l(m_);
    if (raw_motion_) return;  // raw XI2 deltas drive look; warped positions are noise
    const int x = e.x(), y = e.y();
    if (have_prev_) { dx_ += float(x - prev_x_); dy_ += float(y - prev_y_); }
    prev_x_ = x; prev_y_ = y; have_prev_ = true;
  }
  // Raw unaccelerated deltas (X11 XI2 while captured). Once these flow, the
  // position-difference path above is skipped: it double-counts the same
  // motion and re-adds the OS acceleration curve the raw path exists to avoid.
  void OnMouseRelativeMotion(rex::ui::MouseRelativeEvent& e) override {
    std::lock_guard<std::mutex> l(m_);
    raw_motion_ = true;
    dx_ += e.dx(); dy_ += e.dy();
  }
```

In `OnLostFocus` also add `raw_motion_ = false;` next to the delta reset.

- [ ] **Step 2: Float the take functions and callers**

Lines 1138-1139:

```cpp
float ge_take_mouse_dx() { return g_listener.take_dx(); }
float ge_take_mouse_dy() { return g_listener.take_dy(); }
```

Lines 1202-1203 in `ge_mouse_camera` (drop the now-redundant casts):

```cpp
  const float mdx = ge_take_mouse_dx();
  const float mdy = ge_take_mouse_dy();
```

- [ ] **Step 3: Build**

Run: `cd /home/keith/Projects/GoldenEye-Recomp && cmake --build --preset linux-amd64-relwithdebinfo --target ge`
Expected: clean build.

- [ ] **Step 4: Commit (game repo)**

```bash
cd /home/keith/Projects/GoldenEye-Recomp
git add src/ge_hooks.cpp
git commit -m "feat(input): consume raw XI2 mouse deltas, float precision

GameInputListener now accumulates float deltas and consumes the SDK's
new MouseRelativeEvent (unaccelerated XI2 raw motion) when available,
ignoring the warp/position-difference deltas while raw flows - they
double-count the motion and carry the OS acceleration curve. Non-X11
keeps the old fallback, now with float accumulation."
```

---

### Task 7: `ge_mouse_smooth` EMA cvar + menu slider (game)

Optional exponential-moving-average smoothing over the per-frame camera delta, so uneven frame pacing doesn't translate 1:1 into uneven yaw steps. Default 0 (off) — raw feel unchanged unless opted in. Applies to the camera/crosshair look paths only, NOT the menu cursor (a laggy menu cursor feels broken).

**Files:**
- Modify: `/home/keith/Projects/GoldenEye-Recomp/src/ge_hooks.cpp` (cvar block ~line 993; `ge_mouse_camera` after the mdx/mdy take, line ~1204; the `mdx`/`mdy` uses in the aim block lines 1283-1284 and hipfire block lines 1328-1345)
- Modify: `/home/keith/Projects/GoldenEye-Recomp/src/ge_menu.cpp` (CONTROLS tab, after the sensitivity slider block ~line 678)

**Interfaces:**
- Consumes: float `mdx`/`mdy` from Task 6.
- Produces: cvar `ge_mouse_smooth` (double, default 0.0, range 0.0–0.9).

- [ ] **Step 1: Define the cvar**

Next to `ge_mouse_sens` (line 993):

```cpp
REXCVAR_DEFINE_DOUBLE(ge_mouse_smooth, 0.0, "Input",
                      "Mouse look smoothing, EMA over per-frame deltas (0 = off/raw .. 0.9 = heavy)")
    .range(0.0, 0.9);
```

- [ ] **Step 2: Compute smoothed camera deltas in `ge_mouse_camera`**

Directly after the `mdx`/`mdy` take (and before the menu-crosshair block, which keeps using raw `mdx`/`mdy`):

```cpp
  // Optional look smoothing: EMA over the per-frame delta so frame-pacing
  // jitter doesn't turn 1:1 into uneven yaw steps. Camera/crosshair only --
  // the menu cursor above stays raw (a laggy cursor feels broken). The tail
  // snaps to zero once imperceptible so the camera fully stops.
  static float smooth_dx = 0.f, smooth_dy = 0.f;
  const float smooth = static_cast<float>(REXCVAR_GET(ge_mouse_smooth));
  float cdx = mdx, cdy = mdy;
  if (smooth > 0.f) {
    smooth_dx = smooth_dx * smooth + mdx * (1.f - smooth);
    smooth_dy = smooth_dy * smooth + mdy * (1.f - smooth);
    if (mdx == 0.f && std::fabs(smooth_dx) < 0.01f) smooth_dx = 0.f;
    if (mdy == 0.f && std::fabs(smooth_dy) < 0.01f) smooth_dy = 0.f;
    cdx = smooth_dx; cdy = smooth_dy;
  } else {
    smooth_dx = smooth_dy = 0.f;  // don't carry a stale tail into a re-enable
  }
```

Then replace every `mdx`/`mdy` use from the aim block onward with `cdx`/`cdy` (the menu block above keeps `mdx`/`mdy`):
- line 1283: `chX += (invert_x ? -1.f : 1.f) * (cdx / dividor) * sensitivity;`
- line 1284: `chY += (invert_y ? -1.f : 1.f) * (cdy / dividor) * sensitivity;`
- line 1328: `if (cdx != 0.f || cdy != 0.f) {`
- line 1332: `camX += (invert_x ? -1.f : 1.f) * (cdx / 10.f) * sensitivity;`
- line 1335: `const float gun_sway_x = ((cdx / 16000.f) * sensitivity) * bounds;`
- line 1336: `const float gun_sway_y = ((cdy / 16000.f) * sensitivity) * bounds;`
- lines 1341/1343: `camY -= (cdy / 10.f) * sensitivity;` / `camY += (cdy / 10.f) * sensitivity;`

`<cmath>` is already available in this TU (`sqrtf` is used at line 1298); use `std::fabs`.

- [ ] **Step 3: Menu slider in the CONTROLS tab of `ge_menu.cpp`**

After the sensitivity slider's `IsItemDeactivatedAfterEdit` block (~line 679):

```cpp
      // Look smoothing -- EMA over per-frame deltas; 0 = raw/off. Live-read by
      // the look hook each frame; persisted on release like sensitivity.
      float smooth = GetCvarF("ge_mouse_smooth");
      if (ImGui::SliderFloat("Mouse Smoothing", &smooth, 0.0f, 0.9f, "%.2f")) {
        if (smooth < 0.0f) smooth = 0.0f;
        if (smooth > 0.9f) smooth = 0.9f;
        SetCvarF("ge_mouse_smooth", smooth);
      }
      if (ImGui::IsItemDeactivatedAfterEdit() && callbacks_.persist_config)
        callbacks_.persist_config();
```

- [ ] **Step 4: Build**

Run: `cd /home/keith/Projects/GoldenEye-Recomp && cmake --build --preset linux-amd64-relwithdebinfo --target ge`
Expected: clean build.

- [ ] **Step 5: Commit (game repo)**

```bash
cd /home/keith/Projects/GoldenEye-Recomp
git add src/ge_hooks.cpp src/ge_menu.cpp
git commit -m "feat(input): optional ge_mouse_smooth EMA for mouse look

EMA over the per-frame look delta (camera/crosshair paths only; menu
cursor stays raw) so frame-pacing jitter doesn't map 1:1 onto yaw
steps. Default 0 = off; CONTROLS-tab slider, live + persisted."
```

---

### Task 8: Runtime verification on Linux

**Files:** none (verification only)

- [ ] **Step 1: Launch the game with logging**

```bash
cd /home/keith/Projects/GoldenEye-Recomp
LD_LIBRARY_PATH=../GoldenEye-Recomp-rexglue/out/linux-amd64 \
  ./out/linux-amd64-relwithdebinfo/GoldenEye --game_data_root=$PWD/assets \
  --ge_fps_log --log_file=/tmp/ge-verify.log &
```

(If the binary name/path differs, find it with `ls out/linux-amd64-relwithdebinfo/` — the release memory says OUTPUT_NAME is `GoldenEye`.)

- [ ] **Step 2: Verify each fix in the log**

```bash
sleep 40 && grep -E "XI2 raw mouse motion|GEMOUSE|swapchain.*presentation mode|GEFPS|GESHOWN" /tmp/ge-verify.log | head -30
```

Expected:
- `GTKWindow: XI2 raw mouse motion enabled` appears once mouse-look captures (needs a mouse wiggle in-game; may require the user).
- `VulkanPresenter: Created ... presentation mode N` — note N (0=IMMEDIATE, 1=MAILBOX, 2=FIFO, 3=FIFO_RELAXED) for the report.
- GEFPS lines still flowing (logging survived the overlay change).
- No crash on ESC-menu open/close (capture release + overlay callback paths).

- [ ] **Step 3: Interactive checks (hand to the user)**

The feel checks only a human can do — report these as the user's checklist, don't fake them:
1. Mouse-look with `ge_mouse_smooth=0`: should already feel more consistent (no desktop accel curve, sub-pixel fine aim).
2. Slide "Mouse Smoothing" up in the pause menu → progressively smoother/heavier.
3. VIDEO tab: toggle "FPS Overlay" on/off — no crash, overlay appears/disappears.
4. Post-FX enable/disable + preset apply — no crash.
5. On the Ayn Thor later: A/B GESHOWN shown/s with the overlay off (new default) vs on.

- [ ] **Step 4: Final review**

Run `git log --oneline develop..` (game) and `git log --oneline main..` (SDK); confirm 5 commits total match Tasks 2-7. Use superpowers:requesting-code-review, then superpowers:finishing-a-development-branch.
