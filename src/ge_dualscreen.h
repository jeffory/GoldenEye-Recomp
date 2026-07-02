// ge - dual-screen weapon-menu controller (integration glue).
//
// This file is yours to edit. 'rexglue migrate' will NOT overwrite it.
//
// PURPOSE
//   Ties the three pieces of the dual-screen weapon menu together:
//     - the rexglue framework (rex::ui::SecondaryUiSurface, an ImGui-only second
//       surface),
//     - the weapon-menu UI (ge::WeaponMenuDialog),
//     - the platform binding (Android Presentation / desktop test window) that
//       hands us a native window for the secondary display.
//
//   It owns the lifecycle of the secondary surface and drives it. The secondary
//   surface MUST be created, painted and destroyed on the UI thread, but the
//   guest "present" hook that paces us runs on the game thread -- so every frame
//   the game thread asks for one UI-thread tick via CallInUIThreadDeferred and
//   all surface work happens inside that tick.
//
// SINGLE-SCREEN FALLBACK
//   The controller does nothing until a platform binding calls
//   ProvideSecondaryWindow(). On a device with no secondary display (desktop,
//   ordinary phones) that call never happens, so there is zero per-frame cost and
//   no change to the main game. The feature is additionally gated by the
//   `ge_ds_weapon_menu` cvar (pause menu -> VIDEO). Turning it off tears the
//   secondary surface down cleanly and stops all secondary work.
//
// THREADING
//   ProvideSecondaryWindow / RequestSecondaryTeardown / OnSecondaryTouch /
//   OnGuestPresent are callable from ANY thread (they only set atomics / enqueue
//   UI-thread work). Init / Shutdown and everything that touches the surface run
//   on the UI thread.

#pragma once

#include <rex/ui/external_window.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>

namespace rex::ui {
class WindowedAppContext;
class GraphicsProvider;
class SecondaryUiSurface;
}  // namespace rex::ui

namespace ge {

class WeaponMenuDialog;

class DualScreen {
 public:
  static DualScreen& Get();

  // Called once from the UI thread (GeApp::OnCreateDialogs). `provider_getter`
  // returns the primary GraphicsProvider (same one the secondary surface must
  // share); it may return null early in startup and is only invoked on the UI
  // thread once the guest is presenting.
  void Init(rex::ui::WindowedAppContext& app_context,
            std::function<rex::ui::GraphicsProvider*()> provider_getter);

  // UI-thread teardown (GeApp::OnShutdown). Destroys the secondary surface and
  // stops accepting new ticks.
  void Shutdown();

  // Game-thread per-frame pump (from the present hook). Requests at most one
  // outstanding UI-thread tick; cheap no-op when uninitialised.
  void OnGuestPresent();

  // Platform binding -> "here is the secondary display's native window".
  // Callable from any thread. `on_release` (optional) is invoked on the UI
  // thread after the surface built from this handle is destroyed, so the binding
  // can release the native handle it acquired (e.g. ANativeWindow_release).
  void ProvideSecondaryWindow(const rex::ui::ExternalWindowHandle& handle,
                              uint32_t width, uint32_t height,
                              std::function<void()> on_release = {});

  // Platform binding -> "the secondary display went away" (disconnect, pause,
  // Presentation dismissed). Callable from any thread.
  void RequestSecondaryTeardown();

  // Platform binding -> forward a touch from the secondary panel. Callable from
  // any thread; applied on the UI thread. `action` matches
  // rex::ui::TouchEvent::Action (0=down,1=up,2=cancel,3=move).
  void OnSecondaryTouch(uint32_t pointer_id, int action, float x, float y);

 private:
  DualScreen() = default;

  void UiThreadTick();          // UI thread: state machine + paint
  void DestroySurfaceLocked();  // UI thread

  std::atomic<rex::ui::WindowedAppContext*> app_context_{nullptr};
  std::function<rex::ui::GraphicsProvider*()> provider_getter_;

  std::atomic<bool> tick_queued_{false};
  std::atomic<bool> shutdown_{false};
  std::atomic<bool> teardown_requested_{false};

  // Pending native window handed in by the platform binding (guarded by mutex).
  std::mutex pending_mutex_;
  bool have_pending_ = false;
  rex::ui::ExternalWindowHandle pending_handle_{};
  uint32_t pending_w_ = 0;
  uint32_t pending_h_ = 0;
  std::function<void()> pending_release_;

  // UI-thread-owned live state.
  std::unique_ptr<rex::ui::SecondaryUiSurface> surface_;
  WeaponMenuDialog* dialog_ = nullptr;     // owned by surface_'s drawer
  std::function<void()> active_release_;    // release for the live surface's handle
};

}  // namespace ge
