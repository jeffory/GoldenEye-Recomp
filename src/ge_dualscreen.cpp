// ge - dual-screen weapon-menu controller. See ge_dualscreen.h.

#include "ge_dualscreen.h"

#include "ge_weaponmenu.h"

#include <rex/cvar.h>
#include <rex/ui/secondary_ui_surface.h>
#include <rex/ui/ui_event.h>
#include <rex/ui/windowed_app_context.h>

#include <utility>

// Config toggle for the second-screen weapon menu. Lives in the pause menu's
// VIDEO tab (see ge_menu.cpp), persisted to ge.toml. Default ON: on hardware
// with a secondary display (the AYN Thor's bottom panel) the menu appears out
// of the box; single-screen devices never activate it (the platform binding
// hands over no surface), so the default is free there.
//
// History: this was briefly forced OFF (2026-07-02) after the first on-Thor
// bring-up hung the UI thread on the secondary's first paint. Root cause was an
// SDK bug -- SecondaryUiSurface::Create never called immediate_drawer_->
// SetPresenter, so VulkanImmediateDrawer::End() dereferenced a null presenter
// into the guest zero page and spun forever (fixed in the rexglue secondary-
// surface code, not here). The shared Vulkan device is fine. With the SDK fix
// both screens run together (game ~59fps on top, menu on the bottom panel).
REXCVAR_DEFINE_BOOL(ge_ds_weapon_menu, true, "Video",
                    "Show the weapon-selection menu on a connected second screen");

namespace ge {

DualScreen& DualScreen::Get() {
  // Leaked-on-purpose singleton: lives for the whole process so deferred
  // UI-thread ticks posted from the game thread always have a valid target.
  static DualScreen* instance = new DualScreen();
  return *instance;
}

void DualScreen::Init(rex::ui::WindowedAppContext& app_context,
                      std::function<rex::ui::GraphicsProvider*()> provider_getter) {
  provider_getter_ = std::move(provider_getter);
  shutdown_.store(false, std::memory_order_release);
  app_context_.store(&app_context, std::memory_order_release);
}

void DualScreen::Shutdown() {
  // UI thread. Stop accepting ticks first so nothing re-creates the surface.
  shutdown_.store(true, std::memory_order_release);
  app_context_.store(nullptr, std::memory_order_release);
  DestroySurfaceLocked();
  std::lock_guard<std::mutex> lk(pending_mutex_);
  have_pending_ = false;
  if (pending_release_) {
    auto r = std::move(pending_release_);
    pending_release_ = {};
    r();
  }
}

void DualScreen::OnGuestPresent() {
  if (shutdown_.load(std::memory_order_acquire)) {
    return;
  }
  auto* ctx = app_context_.load(std::memory_order_acquire);
  if (!ctx) {
    return;  // not initialised yet
  }
  // Request at most one outstanding UI-thread tick. The tick clears the flag
  // when it FINISHES (see UiThreadTick), so presents that land while a tick is
  // painting are coalesced instead of chaining another tick into the same
  // deferred-queue drain pass (which would starve the UI loop).
  if (tick_queued_.exchange(true, std::memory_order_acq_rel)) {
    return;
  }
  if (!ctx->CallInUIThreadDeferred([] { DualScreen::Get().UiThreadTick(); })) {
    // Context is shutting down and won't run the function; allow a retry.
    tick_queued_.store(false, std::memory_order_release);
  }
}

void DualScreen::ProvideSecondaryWindow(const rex::ui::ExternalWindowHandle& handle,
                                        uint32_t width, uint32_t height,
                                        std::function<void()> on_release) {
  std::function<void()> stale_release;
  {
    std::lock_guard<std::mutex> lk(pending_mutex_);
    // Replace any unconsumed previous pending handle (release it below).
    if (have_pending_ && pending_release_) {
      stale_release = std::move(pending_release_);
    }
    have_pending_ = true;
    pending_handle_ = handle;
    pending_w_ = width;
    pending_h_ = height;
    pending_release_ = std::move(on_release);
  }
  if (stale_release) {
    stale_release();
  }
}

void DualScreen::RequestSecondaryTeardown() {
  teardown_requested_.store(true, std::memory_order_release);
  // Nudge a tick so the teardown is processed even if the game stops presenting.
  auto* ctx = app_context_.load(std::memory_order_acquire);
  if (ctx && !tick_queued_.exchange(true, std::memory_order_acq_rel)) {
    if (!ctx->CallInUIThreadDeferred([] { DualScreen::Get().UiThreadTick(); })) {
      tick_queued_.store(false, std::memory_order_release);
    }
  }
}

void DualScreen::OnSecondaryTouch(uint32_t pointer_id, int action, float x, float y) {
  auto* ctx = app_context_.load(std::memory_order_acquire);
  if (!ctx) {
    return;
  }
  ctx->CallInUIThreadDeferred([pointer_id, action, x, y] {
    DualScreen& self = DualScreen::Get();
    if (!self.surface_) {
      return;
    }
    using Action = rex::ui::TouchEvent::Action;
    Action a;
    switch (action) {
      case 0: a = Action::kDown; break;
      case 1: a = Action::kUp; break;
      case 2: a = Action::kCancel; break;
      default: a = Action::kMove; break;
    }
    rex::ui::TouchEvent e(self.surface_->window(), pointer_id, a, x, y);
    self.surface_->window()->InjectTouchEvent(e);
  });
}

void DualScreen::DestroySurfaceLocked() {
  // The drawer does NOT own its dialogs (it only holds non-owning pointers and
  // removes them in their dtor), so delete the dialog while the drawer is still
  // alive, then drop the surface.
  if (dialog_) {
    delete dialog_;
    dialog_ = nullptr;
  }
  surface_.reset();
  if (active_release_) {
    auto r = std::move(active_release_);
    active_release_ = {};
    r();
  }
}

void DualScreen::UiThreadTick() {
  // Clear the one-outstanding-tick flag at the END of the tick, not the start.
  // The SDK drains its deferred-function queue in a `while (!empty())` loop, so
  // with the flag cleared up-front, a guest present landing DURING the paint
  // below (secondary present can block up to a vsync; presents arrive every
  // ~18ms) re-posts the next tick into the same drain pass -- the drain never
  // empties, and the android_main/UI loop never gets back to ALooper input
  // polling or the primary-window paint. On the Ayn Thor that starved input
  // (ANR), froze the top screen (GESHOWN shown/s=0), and escalated to a
  // system_server watchdog restart. Clearing at the end means a present during
  // the paint is simply coalesced into the next tick instead of chained into
  // this drain (worst case the menu lags one frame).
  struct TickFlagClearer {
    std::atomic<bool>& flag;
    ~TickFlagClearer() { flag.store(false, std::memory_order_release); }
  } tick_flag_clearer{tick_queued_};
  if (shutdown_.load(std::memory_order_acquire)) {
    return;
  }

  // 1) Explicit teardown (display disconnect / pause / dismissed Presentation).
  if (teardown_requested_.exchange(false, std::memory_order_acq_rel)) {
    DestroySurfaceLocked();
  }

  // 2) Config gate (single-screen fallback also rides this: when disabled the
  //    secondary surface is destroyed and we do no per-frame work).
  if (!REXCVAR_GET(ge_ds_weapon_menu)) {
    if (surface_) {
      DestroySurfaceLocked();
    }
    return;
  }

  // 3) Create the surface from a pending native window, if we have one and the
  //    graphics provider is ready.
  if (!surface_) {
    bool have = false;
    rex::ui::ExternalWindowHandle handle{};
    uint32_t w = 0, h = 0;
    std::function<void()> rel;
    {
      std::lock_guard<std::mutex> lk(pending_mutex_);
      if (have_pending_) {
        have = true;
        handle = pending_handle_;
        w = pending_w_;
        h = pending_h_;
        rel = std::move(pending_release_);
        have_pending_ = false;
        pending_release_ = {};
      }
    }
    if (have) {
      auto* ctx = app_context_.load(std::memory_order_acquire);
      auto* provider = provider_getter_ ? provider_getter_() : nullptr;
      if (ctx && provider) {
        surface_ = rex::ui::SecondaryUiSurface::Create(*ctx, *provider, handle, w, h);
        if (surface_) {
          dialog_ = new WeaponMenuDialog(surface_->imgui_drawer());
          active_release_ = std::move(rel);
        } else if (rel) {
          rel();  // creation failed: release the native handle now
        }
      } else {
        // Provider not up yet -> put it back and retry on a later tick.
        std::lock_guard<std::mutex> lk(pending_mutex_);
        if (!have_pending_) {
          have_pending_ = true;
          pending_handle_ = handle;
          pending_w_ = w;
          pending_h_ = h;
          pending_release_ = std::move(rel);
        } else if (rel) {
          rel();
        }
      }
    }
  }

  // 4) Paint the live secondary surface.
  if (surface_) {
    surface_->Paint();
  }
}

}  // namespace ge
