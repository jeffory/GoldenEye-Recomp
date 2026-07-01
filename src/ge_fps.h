// ge - ReXGlue Recompiled Project
//
// Guest-FPS benchmark recorder + on-screen overlay. Measures the guest's frame
// PRODUCTION cadence (the same "real frame drawn" signal the GEGPU rendered#
// heartbeat uses -- NOT the host paint/display rate, which can be lower if the
// present path drops frames), coalescing-corrected and with load-screen gaps
// excluded. Reports average / 1%-low / worst FPS plus hitch and gap counts,
// on screen and as grep-able GEFPS lines in ge.log. Built to A/B performance
// changes on the handheld.
//
// This file is yours to edit. 'rexglue migrate' will NOT overwrite it.

#pragma once

#include <rex/ui/imgui_dialog.h>

#include <cstdint>

namespace ge {

// Called exactly once per observed submit-counter advance (from the CAS-winner
// branch in ge_dbg_now). frames_advanced is how many frames the submit counter
// moved by since the last observation (>=1): when a poll misses an intermediate
// value the elapsed time covers several frames, and dividing by the advance
// keeps per-frame stats honest instead of recording one doubled "slow" frame.
// Lock-free; safe to call from the guest frame-wait thread(s). No-op cost when
// neither ge_fps_overlay nor ge_fps_log is set is still one timestamp + a few
// atomics -- negligible per frame.
void FpsOnFrame(uint32_t frames_advanced);

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
