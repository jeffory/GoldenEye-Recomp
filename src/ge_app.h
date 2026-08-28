
// ge - ReXGlue Recompiled Project
//
// This file is yours to edit. 'rexglue migrate' will NOT overwrite it.
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include <rex/cvar.h>
#include <rex/graphics/graphics_system.h>
#include <rex/logging/macros.h>
#include <rex/perf/counter.h>
#include <rex/rex_app.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xam/user_profile.h>
#include <rex/ui/keybinds.h>
#include <rex/ui/window.h>
#include <rex/ui/windowed_app_context.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <thread>

#include "ge_dualscreen.h"
#include "ge_fps.h"
#include "ge_menu.h"
#include "ge_asset_check.h"
#include "ge_postfx.h"
#include "ge_input_map.h"
#include "ge_replay.h"
#include "ge_touchpad.h"

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
    // Register the dual-screen JNI methods with ART before the Java side can
    // call them (it additionally gates on a live render loop). See
    // ge_android_ds.cpp for why System.loadLibrary can't do this.
    ge::AndroidDsRegisterNatives();
    // Register the on-screen touch-controls JNI methods too (same reasoning:
    // RegisterNatives, not System.loadLibrary). The Java overlay gates its first
    // call on a live render loop, long after this runs.
    ge::AndroidTouchRegisterNatives();
    // No config file / CLI on Android: turn the guest-FPS benchmark recorder on
    // here so the on-screen readout + periodic GEFPS ge.log lines are available
    // for measuring framerate on the handheld. (Desktop leaves these default-off
    // and toggles them with --ge_fps_overlay / --ge_fps_log.)
    // GEFPS logging stays on (it needs no UI drawer), but the on-screen
    // overlay now defaults OFF: a registered overlay dialog pins every
    // present to the UI thread (see UpdateOverlayRegistration), which on the
    // handheld quantizes the shown rate down (GESHOWN "22 shown / 52
    // produced"). Toggle it per-session from the pause menu VIDEO tab.
    rex::cvar::SetFlagByName("ge_fps_log", "true");
    // Spike attribution lines (GESPIKE) on by default on the handheld -- rate-
    // limited to ~4/s and only emitted when a frame exceeds 2x the median.
    rex::cvar::SetFlagByName("ge_spike_log", "true");
    // GPU execution time via Vulkan timestamp queries (kGpuFrameUs -> the
    // GESPIKE gpu= column + the overlay's gpu bar). Cost: one TOP/BOTTOM
    // timestamp pair per submission + a no-wait readback. Validated on the
    // Thor's Adreno (and on desktop, period 10ns); the code self-disables on
    // devices whose queue family lacks timestamp support.
    rex::cvar::SetFlagByName("ge_gpu_timestamps", "true");
    // Pad-first handheld: keep the xenia-canary mouse-look port OFF. It defaults
    // on, and with it ge_disable_autoaim strips auto-aim/look-ahead on every
    // pause/cutscene transition and the crosshair/gun-centering writes run every
    // frame with no mouse attached (ge_mouse_camera in ge_hooks.cpp) -- all
    // unverified on arm64. Gating ge_mouselook_enable skips that whole path
    // (ge_disable_autoaim is only read inside it); the CE data patches are
    // applied before the gate and are unaffected. Re-enable here once the port
    // has had a Thor pass (or gate it on real mouse motion instead).
    rex::cvar::SetFlagByName("ge_mouselook_enable", "false");
    // Game data root chosen on-device by GameSetupActivity (issue #16).
    //
    // Android 11+ stops ordinary file managers writing into
    // Android/data/<pkg>/files, so staging the ~700 MB dump there needs a PC
    // (adb push) or root. Instead the Java setup activity takes "All files
    // access" (MANAGE_EXTERNAL_STORAGE) plus a folder picker and records the
    // chosen directory here; we point the guest VFS straight at it, so the dump
    // is read in place with no second copy.
    //
    // Only game_data_root moves. log_file / cache_path / user_data_root are
    // separate cvars set by android_main and must stay in app-private storage
    // (they have to be writable, and the chosen folder may not be).
    //
    // No logging in this function: it runs before rex::InitLogging(), so
    // REXKRNL_* here is swallowed by the early stdout-only logger and never
    // reaches ge.log (see OnPostInitLogging). The outcome is stashed and logged
    // there instead.
    android_game_root_note_ = ApplyChosenGameRoot(paths);
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

  // Runs right after rex::InitLogging(), still early in SetupEnvironment --
  // well before SetupPresentation touches init-only cvars like
  // render_target_path_vulkan. Deliberately NOT called from the end of
  // OnConfigurePaths above: that runs *before* rex::InitLogging() (see
  // ReXApp::SetupEnvironment), so any REXKRNL_* logging done from there is
  // silently served by a lazily-created early stdout-only logger and never
  // reaches --log_file / ge.log -- verified empirically (the GECVAR lines
  // showed up on stdout but never in the log file) before moving the call
  // here. user_data_root() is used instead of a PathConfig& (this hook takes
  // none) -- it is populated from the same path_config right after
  // OnConfigurePaths returns, so it holds the identical value.
  void OnPostInitLogging() override {
#if defined(__ANDROID__)
    // Deferred from OnConfigurePaths (which runs before logging exists).
    if (!android_game_root_note_.empty()) {
      REXKRNL_INFO("GEGAMEROOT {}", android_game_root_note_);
    }
#endif
    // Last, so a pushed file can override every default set in
    // OnConfigurePaths above. See ApplyCvarOverrides for why this exists.
    ApplyCvarOverrides(user_data_root() / "ge_cvars.txt");
  }

  // Register the ESC pause-menu keybind and (conditionally) the overlay
  // dialogs once the ImGui drawer exists.
  void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {
    // Window/taskbar title shown while running. Overrides the SDK default
    // ("ge <build stamp>"); the internal app name stays "ge" so ge.toml and the
    // user data dir are unchanged.
    if (window()) window()->SetTitle("GoldenEye");
    rex::ui::RegisterBind("bind_pause_menu", "Escape", "Pause menu",
                          [this] { TogglePauseMenu(); });
    // Controller route into the same menu. This is the only way in on Android:
    // there is no keyboard for the ESC bind above, and the platform layer
    // routes every input event to the gamepad driver, so touch never reaches
    // ImGui either. Fires on the guest input thread -- hop to the UI thread,
    // where the dialog list lives.
    ge::inputmap::SetMenuChordCallback([this] {
      app_context().CallInUIThreadDeferred([this] { TogglePauseMenu(); });
    });
    ge::InitMouseLook();  // attach the cross-platform mouse/keyboard look listener
    drawer_ = drawer;
    UpdateOverlayRegistration();  // overlays exist only while their cvar is on
    // F2 starts a fresh benchmark window (clears avg / 1%-low / min / max).
    rex::ui::RegisterBind("bind_fps_reset", "F2", "Reset FPS benchmark",
                          [] { ge::FpsReset(); });
    // Username/server are set in the ONLINE pause-menu tab now -- no first-boot
    // prompt. They apply on the Save & Restart the ONLINE tab triggers.

    // Wire up the dual-screen weapon menu. This only arms the controller; it
    // stays completely inactive until a platform binding reports a secondary
    // display (single-screen fallback). The provider getter is invoked later, on
    // the UI thread, once the guest is presenting -- runtime()/graphics_system()
    // are live by then.
    ge::DualScreen::Get().Init(app_context(), [this]() -> rex::ui::GraphicsProvider* {
      auto* rt = runtime();
      if (!rt) return nullptr;
      auto* igs = rt->graphics_system();
      if (!igs) return nullptr;
      return static_cast<rex::graphics::GraphicsSystem*>(igs)->provider();
    });
  }

  // Tear down the menu, overlay and keybind before the drawer is destroyed.
  void OnShutdown() override {
    ge::inputmap::SetMenuChordCallback(nullptr);
    ge::inputmap::SetGuestInputSuppressed(false);
    rex::ui::UnregisterBind("bind_pause_menu");
    rex::ui::UnregisterBind("bind_fps_reset");
    fps_overlay_.reset();
    // Tear the secondary surface down on the UI thread before the drawer/graphics
    // go away.
    ge::DualScreen::Get().Shutdown();
    if (menu_) {
      // Direct delete (not Close()) so we don't re-enter pause bookkeeping
      // during shutdown; removes itself from the drawer in its destructor.
      delete menu_;
      menu_ = nullptr;
    }
    postfx_.reset();
  }

  // Called on the UI thread immediately before the main guest thread starts.
  // Verify the game dump against the generated manifest so a broken install
  // produces a clear error (instead of the guest faulting on the first file it
  // actually needs). Returning false vetoes the guest launch.
  bool OnPreLaunchModule() override {
    // Compile the gamepad remap before the first poll: ReplayOnGetState applies
    // it, so it has to be live by the time that override is armed below.
    ge::inputmap::Init();

    // Install the input record/replay/bench harness before the guest starts
    // polling gamepad state. quit_requester mirrors the pause menu's on_quit
    // (TogglePauseMenu(), below) deferred to the UI thread -- this can be
    // invoked from a guest thread once ge_bench_exit finishes a replay.
    ge::ReplayInit(user_data_root(), [this] {
      app_context().CallInUIThreadDeferred([this] {
        if (runtime() && runtime()->kernel_state()) {
          runtime()->kernel_state()->TerminateTitle();
        }
        // Mirror ReXApp::OnClosing's hard-exit tail (SDK src/ui/rex_app.cpp)
        // instead of QuitFromUIThread(): normal subsystem teardown can
        // deadlock on a host lock still held by a straggler guest thread that
        // TerminateTitle's cooperative drain deliberately leaves running.
        // ge_bench_exit exists so an on-device run can quit itself
        // unattended -- QuitFromUIThread() alone does not guarantee that.
        if (runtime() && runtime()->graphics_system()) {
          runtime()->graphics_system()->PersistCaches();
        }
        rex::FlushLogging();
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        std::_Exit(0);
      });
    });
    return ge::RunStartupAssetCheck(game_data_root(), user_data_root(),
                                    app_context());
  }

 private:
#if defined(__ANDROID__)
  // Point paths.game_data_root at the folder GameSetupActivity recorded in
  // <user_data_root>/ge_game_path.txt (one line, an absolute host path).
  //
  // Returns a short human-readable note describing what happened, for
  // OnPostInitLogging to emit -- this runs before logging is initialised.
  //
  // Absent file / unreadable folder => leave the default (the app's external
  // files dir) alone, so installs staged there by adb keep booting untouched
  // and a stale entry degrades to the old behaviour instead of a hard failure.
  static std::string ApplyChosenGameRoot(rex::PathConfig& paths) {
    if (paths.user_data_root.empty()) {
      return "no user_data_root; keeping default game data root";
    }
    const auto file = paths.user_data_root / "ge_game_path.txt";
    std::error_code ec;
    if (!std::filesystem::exists(file, ec) || ec) {
      return "no " + file.string() + "; using default " + paths.game_data_root.string();
    }
    std::ifstream in(file);
    if (!in) {
      return "could not open " + file.string() + "; using default " +
             paths.game_data_root.string();
    }
    std::string chosen;
    std::getline(in, chosen);
    // Trim whitespace/newlines a text editor or `adb push` may have left.
    const char* ws = " \t\r\n";
    const auto b = chosen.find_first_not_of(ws);
    if (b == std::string::npos) {
      return "empty " + file.string() + "; using default " + paths.game_data_root.string();
    }
    chosen = chosen.substr(b, chosen.find_last_not_of(ws) - b + 1);

    // Must be a readable directory NOW. Storage the picker could reach can
    // disappear (SD card pulled, permission revoked in Settings); falling back
    // keeps the boot path identical to a fresh install, and the startup asset
    // check then produces the normal "missing files" screen.
    if (!std::filesystem::is_directory(chosen, ec) || ec) {
      return "chosen game data root is not a readable directory: " + chosen +
             "; using default " + paths.game_data_root.string();
    }
    paths.game_data_root = chosen;
    return "using chosen game data root " + chosen;
  }

  // Outcome of the above, logged from OnPostInitLogging.
  std::string android_game_root_note_;
#endif

  // Apply "name=value" cvar overrides from a text file, if one exists.
  //
  // Android reads no config file and takes no CLI, so cvars are otherwise
  // hardcoded in OnConfigurePaths below -- and init-only cvars like
  // render_target_path_vulkan then cost a full rebuild + reinstall to change.
  // This lets `adb push` swap a config between runs instead. Applied last so it
  // beats the hardcoded defaults; on desktop, CLI flags still win as usual.
  //
  // Format: one name=value per line. '#' starts a comment. Blank lines and
  // lines without '=' are skipped. Unknown cvar names are logged and ignored.
  static void ApplyCvarOverrides(const std::filesystem::path& file) {
    std::error_code ec;
    if (!std::filesystem::exists(file, ec) || ec) {
      // Logged (not silent): a wrong path or a failed `adb push` would
      // otherwise be indistinguishable from "feature not built in", and the
      // Phase-0 protocol's own recovery instructions tell the operator to
      // read the resolved path back out of a GECVAR line -- which this early
      // return would otherwise suppress entirely.
      REXKRNL_INFO("GECVAR no override file at {} (0 overrides)", file.string());
      return;
    }
    std::ifstream in(file);
    if (!in) {
      REXKRNL_WARN("GECVAR override file exists but could not be opened: {}", file.string());
      return;
    }
    auto trim = [](std::string& s) {
      const char* ws = " \t\r\n";
      const auto b = s.find_first_not_of(ws);
      if (b == std::string::npos) {
        s.clear();
        return;
      }
      s = s.substr(b, s.find_last_not_of(ws) - b + 1);
    };
    std::string line;
    int applied = 0;
    while (std::getline(in, line)) {
      const auto hash = line.find('#');
      if (hash != std::string::npos) {
        line.erase(hash);
      }
      const auto eq = line.find('=');
      if (eq == std::string::npos) {
        continue;
      }
      std::string name = line.substr(0, eq);
      std::string value = line.substr(eq + 1);
      trim(name);
      trim(value);
      if (name.empty()) {
        continue;
      }
      if (rex::cvar::SetFlagByName(name, value)) {
        REXKRNL_INFO("GECVAR override applied: {}={}", name, value);
        ++applied;
      } else {
        REXKRNL_WARN("GECVAR override rejected (unknown cvar or bad value): {}={}", name, value);
      }
    }
    REXKRNL_INFO("GECVAR applied {} override(s) from {}", applied, file.string());
  }

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

  // ESC handler: open or close the menu. The game keeps running underneath.
  void TogglePauseMenu() {
    // Logged because on Android the controller chord is the only way in, and a
    // silent no-op here is indistinguishable from the chord never firing.
    REXKRNL_INFO("GEMENU toggle: {} (drawer={})", menu_ ? "closing" : "opening",
                 imgui_drawer() ? "yes" : "null");
    if (menu_) {
      menu_->RequestClose();  // on_closed clears menu_
      return;
    }
    GeMenuDialog::Callbacks cb;
    cb.on_closed = [this] {
      menu_ = nullptr;
      ge::SetMouselookSuppressed(false);  // re-enable mouse-look on menu close
      ge::inputmap::SetGuestInputSuppressed(false);
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
    cb.overlays_changed = [this] {
      // Deferred: the menu invokes this from inside the drawer's draw, and
      // creating/destroying dialogs mid-draw is the same hazard as the
      // fullscreen switch above.
      app_context().CallInUIThreadDeferred([this] { UpdateOverlayRegistration(); });
    };
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
    // The menu is driven with the same controller the game is reading, so the
    // game must stop seeing it -- otherwise navigating the menu also plays.
    ge::inputmap::SetGuestInputSuppressed(true);
    menu_ = new GeMenuDialog(imgui_drawer(), std::move(cb));
  }

  GeMenuDialog* menu_ = nullptr;  // non-owning; self-deletes via the drawer
  rex::ui::ImGuiDrawer* drawer_ = nullptr;          // set once in OnCreateDialogs
  std::unique_ptr<ge::PostFxOverlay> postfx_;       // filter layer (alive only while enabled)
  std::unique_ptr<ge::FpsOverlay> fps_overlay_;     // guest-FPS readout (alive only while enabled)
  static inline bool ge_perf_csv_on_ = false;       // perf-CSV capture running?
};
