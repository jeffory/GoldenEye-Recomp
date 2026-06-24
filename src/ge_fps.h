// ge - ReXGlue Recompiled Project
//
// Lightweight FPS / frametime corner overlay and perf log.
//
// FpsRecordPresent() is called once per guest present (from ge_diag_vdswap)
// with the current REX_QUERY_TIMEBASE() tick. FpsOverlay is an always-on
// ImGui dialog that reads the ring buffer and draws a small HUD.
//
// Gated behind ge_show_fps (default off). ge_perf_log (default off) writes
// frametime percentiles to ge.log ~once/sec for offline before/after comparison.
//
// This file is yours to edit. 'rexglue migrate' will NOT overwrite it.

#pragma once

#include <rex/ui/imgui_dialog.h>

#include <cstdint>

namespace ge {

// Record a present timestamp (REX_QUERY_TIMEBASE() tick) into the ring buffer.
// Called from ge_diag_vdswap() -- the natural per-present hook point.
// Thread-safe; relaxed atomics (diagnostic accuracy is sufficient).
void FpsRecordPresent(uint32_t tb);

// Corner HUD overlay: FPS, frametime (ms), rolling min/avg/max.
// Created once at startup alongside PostFxOverlay; renders when ge_show_fps=true.
class FpsOverlay : public rex::ui::ImGuiDialog {
 public:
  explicit FpsOverlay(rex::ui::ImGuiDrawer* drawer);
  ~FpsOverlay() override;

 protected:
  void OnDraw(ImGuiIO& io) override;
};

}  // namespace ge
