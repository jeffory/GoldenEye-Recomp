
// ge - ReXGlue Recompiled Project
//
// This file is yours to edit. 'rexglue migrate' will NOT overwrite it.
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include <rex/cvar.h>
#include <rex/perf/counter.h>
#include <rex/rex_app.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xam/user_profile.h>
#include <rex/ui/keybinds.h>
#include <rex/ui/window.h>
#include <rex/ui/windowed_app_context.h>

#include <string>

#include "ge_fps.h"
#include "ge_menu.h"
#include "ge_postfx.h"

// Relaunch the current executable as a fresh process (implemented in
// ge_hooks.cpp, which owns the Win32 includes). Used by the ONLINE menu's
// "Save & Restart" so username/server/enable changes take effect on a clean
// boot -- they are read at startup (UserProfile ctor, online client start).
namespace ge {
void LaunchSelfDetached();
// Attach the cross-platform mouse/keyboard look listener at startup (implemented
// in ge_hooks.cpp).
void InitMouseLook();
// Suppress mouse-look while the pause menu is open (cursor is needed for the
// menu, and motion shouldn't turn into look). Implemented in ge_hooks.cpp.
void SetMouselookSuppressed(bool suppressed);
}

class GeApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<GeApp>(new GeApp(ctx, "ge",
        PPCImageConfig));
  }

  // GoldenEye boot defaults. Runs before the config file is loaded, so these
  // are just defaults -- ge.toml (written by the in-game menu) overrides them.
  void OnConfigurePaths(rex::PathConfig& paths) override {
    (void)paths;
    // NOTE: vsync is NOT forced here. Its SDK default is false (off), so the
    // in-menu toggle persists: turning it ON differs from default -> written to
    // ge.toml; OFF == default -> not written but still boots off. Forcing it here
    // would re-assert off every boot and the "on" choice would never survive a
    // restart (SaveConfig only writes cvars that differ from their default).
    rex::cvar::SetFlagByName("max_fps", "60");  // default 60 (clamped to native refresh)
    rex::cvar::SetFlagByName("window_width", "2560");
    rex::cvar::SetFlagByName("window_height", "1440");
    // Our 1:1 mouse-look + keyboard injection (ge_hooks.cpp) is the sole MnK
    // path. Force the SDK's mouse-as-stick driver off so a stale ge.toml can't
    // re-enable it alongside ours (double-input / cursor fight).
    rex::cvar::SetFlagByName("mnk_mode", "false");
#if defined(__ANDROID__)
    // No config file / CLI on Android: turn the guest-FPS benchmark recorder on
    // here so the on-screen readout + periodic GEFPS ge.log lines are available
    // for measuring framerate on the handheld. (Desktop leaves these default-off
    // and toggles them with --ge_fps_overlay / --ge_fps_log.)
    rex::cvar::SetFlagByName("ge_fps_overlay", "true");
    rex::cvar::SetFlagByName("ge_fps_log", "true");
    // Pad-first handheld: keep the xenia-canary mouse-look port OFF. It defaults
    // on, and with it ge_disable_autoaim strips auto-aim/look-ahead on every
    // pause/cutscene transition and the crosshair/gun-centering writes run every
    // frame with no mouse attached (ge_mouse_camera in ge_hooks.cpp) -- all
    // unverified on arm64. Gating ge_mouselook_enable skips that whole path
    // (ge_disable_autoaim is only read inside it); the CE data patches are
    // applied before the gate and are unaffected. Re-enable here once the port
    // has had a Thor pass (or gate it on real mouse motion instead).
    rex::cvar::SetFlagByName("ge_mouselook_enable", "false");
#endif
    // NOTE: fullscreen is NOT forced here. Its default is set to true at the
    // framework level (window.cpp) instead. That makes "windowed" the
    // non-default value, so toggling to windowed actually saves to ge.toml --
    // SaveConfig only writes cvars that differ from their default. Forcing
    // fullscreen=true here would re-assert it every boot and the windowed
    // choice would never persist. The throttle is the same story: its default
    // lives in its REXCVAR_DEFINE and it is tuned live from the pause menu, so
    // it is never written here (writing default==default is a no-op anyway).
  }

  // Register the ESC pause-menu keybind and create the always-on Post-FX
  // filter overlay once the ImGui drawer exists.
  void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {
    // Window/taskbar title shown while running. Overrides the SDK default
    // ("ge <build stamp>"); the internal app name stays "ge" so ge.toml and the
    // user data dir are unchanged.
    if (window()) window()->SetTitle("GoldenEye");
    rex::ui::RegisterBind("bind_pause_menu", "Escape", "Pause menu",
                          [this] { TogglePauseMenu(); });
    ge::InitMouseLook();  // attach the cross-platform mouse/keyboard look listener
    postfx_ = std::make_unique<ge::PostFxOverlay>(drawer);
    fps_overlay_ = std::make_unique<ge::FpsOverlay>(drawer);  // guest-FPS readout
    // F2 starts a fresh benchmark window (clears avg / 1%-low / min / max).
    rex::ui::RegisterBind("bind_fps_reset", "F2", "Reset FPS benchmark",
                          [] { ge::FpsReset(); });
    // Username/server are set in the ONLINE pause-menu tab now -- no first-boot
    // prompt. They apply on the Save & Restart the ONLINE tab triggers.
  }

  // Tear down the menu, overlay and keybind before the drawer is destroyed.
  void OnShutdown() override {
    rex::ui::UnregisterBind("bind_pause_menu");
    rex::ui::UnregisterBind("bind_fps_reset");
    fps_overlay_.reset();
    if (menu_) {
      // Direct delete (not Close()) so we don't re-enter pause bookkeeping
      // during shutdown; removes itself from the drawer in its destructor.
      delete menu_;
      menu_ = nullptr;
    }
    postfx_.reset();
  }

 private:
  // ESC handler: open or close the menu. The game keeps running underneath.
  void TogglePauseMenu() {
    if (menu_) {
      menu_->RequestClose();  // on_closed clears menu_
      return;
    }
    GeMenuDialog::Callbacks cb;
    cb.on_closed = [this] {
      menu_ = nullptr;
      ge::SetMouselookSuppressed(false);  // re-enable mouse-look on menu close
    };
    cb.on_quit = [this] {
      if (runtime() && runtime()->kernel_state()) {
        runtime()->kernel_state()->TerminateTitle();
      }
      app_context().QuitFromUIThread();
    };
    cb.get_fullscreen = [this] { return window() && window()->IsFullscreen(); };
    cb.request_fullscreen = [this](bool v) {
      // Persist the choice: update the cvar (so SaveConfig writes it) and flush
      // ge.toml now. Without this the window changes but reverts next boot.
      rex::cvar::SetFlagByName("fullscreen", v ? "true" : "false");
      PersistConfig();
      // Defer off the paint thread: applying a window/surface change from inside
      // the ImGui draw (which runs during the presenter's paint) tears down the
      // surface being painted and crashes. Running it from the UI loop between
      // frames is the same safe path as a normal window resize.
      app_context().CallInUIThreadDeferred([this, v] {
        if (window()) window()->SetFullscreen(v);
      });
    };
    cb.persist_config = [this] { PersistConfig(); };
    // Perf CSV capture (VIDEO tab checkbox). Opt-in per session -- the writer
    // + its periodic fflush run on the CP worker, so it is never left on by
    // default. Lands next to ge.log in the user data dir; pull with adb and
    // feed to scripts/perf_report.py.
    cb.get_perf_csv = [] { return ge_perf_csv_on_; };
    cb.set_perf_csv = [this](bool on) {
      ge_perf_csv_on_ = on;
      rex::perf::SetCsvLogPath(
          on ? (user_data_root() / "ge_perf.csv").string() : std::string());
    };
    cb.request_restart = [this] {
      // ONLINE tab "Save & Restart": the menu has already persisted the cvars;
      // launch a fresh process (which reads the new ge.toml at boot) then tear
      // this one down. Deferred to the UI thread -- never quit/relaunch from
      // inside the paint (same reason as request_fullscreen).
      app_context().CallInUIThreadDeferred([this] {
        ge::LaunchSelfDetached();
        if (runtime() && runtime()->kernel_state()) {
          runtime()->kernel_state()->TerminateTitle();
        }
        app_context().QuitFromUIThread();
      });
    };
    ge::SetMouselookSuppressed(true);  // freeze mouse-look + free the cursor while the menu is up
    menu_ = new GeMenuDialog(imgui_drawer(), std::move(cb));
  }

  GeMenuDialog* menu_ = nullptr;  // non-owning; self-deletes via the drawer
  std::unique_ptr<ge::PostFxOverlay> postfx_;       // always-on filter layer
  std::unique_ptr<ge::FpsOverlay> fps_overlay_;     // guest-FPS benchmark readout
  static inline bool ge_perf_csv_on_ = false;       // perf-CSV capture running?
};
