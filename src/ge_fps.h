// ge - ReXGlue Recompiled Project
//
// Guest-FPS benchmark recorder + on-screen overlay. Measures the rate at which
// the guest actually renders frames (the same "real frame drawn" signal the
// GEGPU rendered# heartbeat uses -- NOT the host ImGui paint rate), and reports
// average / 1%-low / worst / best FPS both on screen and as grep-able GEFPS
// lines in ge.log. Built to A/B performance changes on the handheld.
//
// This file is yours to edit. 'rexglue migrate' will NOT overwrite it.

#pragma once

#include <rex/ui/imgui_dialog.h>

namespace ge {

// Called exactly once per genuinely-rendered guest frame (from the submit-
// advance branch in ge_dbg_now). Lock-free; safe to call from the guest frame-
// wait thread(s). No-op cost when neither ge_fps_overlay nor ge_fps_log is set
// is still one timestamp + a few atomics -- negligible per frame.
void FpsOnFrame();

// Start a fresh measurement window (clears avg / 1%-low / min / max / count).
// Bound to a desktop keybind and called once when the recorder is first enabled.
void FpsReset();

// Small always-on-top text readout (live FPS + avg / 1%-low / worst / best /
// frame count). Created once at startup like PostFxOverlay; gated each frame by
// the ge_fps_overlay cvar.
class FpsOverlay : public rex::ui::ImGuiDialog {
 public:
  explicit FpsOverlay(rex::ui::ImGuiDrawer* drawer);
  ~FpsOverlay() override;

 protected:
  void OnDraw(ImGuiIO& io) override;
};

}  // namespace ge
