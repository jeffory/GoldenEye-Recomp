// ge - ReXGlue Recompiled Project
//
// Lightweight FPS/frametime corner overlay + optional perf log.
// See ge_fps.h for the public API.

#include "ge_fps.h"

#include <rex/cvar.h>
#include <rex/hook.h>  // REXKRNL_INFO

#include <imgui.h>

#include <algorithm>
#include <atomic>
#include <cstdio>

// Gate the corner overlay: default OFF so it doesn't intrude on normal play.
// Toggle from the VIDEO tab of the ESC pause menu, or set ge_show_fps=true in
// ge.toml. No rebuild required -- the cvar is hot-reloaded.
REXCVAR_DEFINE_BOOL(ge_show_fps, false, "Video",
                    "GoldenEye: show FPS/frametime corner overlay (top-left HUD)");

// Optional perf log: writes P50/P90/P99 frametime percentiles to ge.log ~once/sec.
// Useful for offline before/after comparison across perf patches.
REXCVAR_DEFINE_BOOL(ge_perf_log, false, "Video",
                    "GoldenEye: log frametime percentiles to ge.log ~once/second");

namespace ge {

namespace {

// Xbox 360 PPC timebase frequency (~49.875 MHz, same source as mftb).
constexpr float kTbHz = 49875000.0f;

// Circular buffer of raw REX_QUERY_TIMEBASE() ticks at each guest present.
// 128 slots ~ 2 seconds at 60 fps. Ring slots are individually atomic; the
// head index is written after the slot, so a racing reader may see a stale
// slot -- tolerated for a diagnostic display.
constexpr int kRingSize = 128;
std::atomic<uint32_t> g_ring[kRingSize];
std::atomic<int>      g_head{0};   // next write slot
std::atomic<int>      g_count{0};  // valid entries clamped to kRingSize

// Timebase tick of the last perf-log line (for ~1s throttle).
std::atomic<uint32_t> g_last_log_tb{0};

// Collect up to `max_deltas` frametime values (ms) from the ring into `out`.
// Returns the count written. Skips any delta that looks obviously wrong
// (0 ticks or > 1 second).
int CollectDeltas(float* out, int max_deltas) {
  const int n = g_count.load(std::memory_order_relaxed);
  if (n < 2) return 0;
  const int head = g_head.load(std::memory_order_relaxed);
  const int pairs = std::min(n - 1, kRingSize - 1);
  int nd = 0;
  for (int i = 0; i < pairs && nd < max_deltas; ++i) {
    const int idx_new = (head - 1 - i + kRingSize * 2) % kRingSize;
    const int idx_old = (head - 2 - i + kRingSize * 2) % kRingSize;
    const uint32_t tnew = g_ring[idx_new].load(std::memory_order_relaxed);
    const uint32_t told = g_ring[idx_old].load(std::memory_order_relaxed);
    const uint32_t dt = tnew - told;
    if (dt == 0 || dt > 49875000u) continue;  // skip zeros and > 1s gaps
    out[nd++] = static_cast<float>(dt) * 1000.0f / kTbHz;
  }
  return nd;
}

}  // namespace

void FpsRecordPresent(uint32_t tb) {
  const int h = g_head.load(std::memory_order_relaxed);
  g_ring[h].store(tb, std::memory_order_relaxed);
  g_head.store((h + 1) % kRingSize, std::memory_order_relaxed);
  const int cnt = g_count.load(std::memory_order_relaxed);
  if (cnt < kRingSize) g_count.store(cnt + 1, std::memory_order_relaxed);

  // Perf log: emit percentile summary ~once/second when ge_perf_log is on.
  if (!REXCVAR_GET(ge_perf_log)) return;
  const uint32_t last_log = g_last_log_tb.load(std::memory_order_relaxed);
  if ((uint32_t)(tb - last_log) < 49875000u) return;  // < ~1 second
  g_last_log_tb.store(tb, std::memory_order_relaxed);

  float deltas[kRingSize - 1];
  const int nd = CollectDeltas(deltas, kRingSize - 1);
  if (nd < 4) return;

  std::sort(deltas, deltas + nd);
  float sum = 0.0f;
  for (int i = 0; i < nd; ++i) sum += deltas[i];
  const float avg = sum / static_cast<float>(nd);
  const float p50 = deltas[nd / 2];
  const float p90 = deltas[(nd * 9) / 10];
  const float p99 = deltas[(nd * 99) / 100];
  const float fps_avg = avg > 0.0f ? 1000.0f / avg : 0.0f;
  REXKRNL_INFO(
      "GE PERF: frames={} avg_fps={:.1f} avg={:.2f}ms p50={:.2f}ms p90={:.2f}ms p99={:.2f}ms",
      nd, fps_avg, avg, p50, p90, p99);
}

FpsOverlay::FpsOverlay(rex::ui::ImGuiDrawer* drawer) : rex::ui::ImGuiDialog(drawer) {}
FpsOverlay::~FpsOverlay() = default;

void FpsOverlay::OnDraw(ImGuiIO& /*io*/) {
  if (!REXCVAR_GET(ge_show_fps)) return;

  // Read up to 64 recent deltas (about 1 second at 60 fps) for min/avg/max.
  // The last delta in the array (index 0) is the most recent frametime.
  float deltas[64];
  const int nd = CollectDeltas(deltas, 64);
  if (nd == 0) return;

  const float ft_last = deltas[0];
  const float fps = ft_last > 0.0f ? 1000.0f / ft_last : 0.0f;

  float mn = ft_last, mx = ft_last, sum = 0.0f;
  for (int i = 0; i < nd; ++i) {
    if (deltas[i] < mn) mn = deltas[i];
    if (deltas[i] > mx) mx = deltas[i];
    sum += deltas[i];
  }
  const float avg = sum / static_cast<float>(nd);

  // Small semi-transparent window pinned to the top-left corner.
  ImGui::SetNextWindowPos(ImVec2(10.0f, 10.0f), ImGuiCond_Always);
  ImGui::SetNextWindowBgAlpha(0.65f);
  constexpr ImGuiWindowFlags kFlags =
      ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
      ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
      ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoInputs |
      ImGuiWindowFlags_NoBringToFrontOnFocus;

  if (!ImGui::Begin("##ge_fps_hud", nullptr, kFlags)) {
    ImGui::End();
    return;
  }

  ImGui::SetWindowFontScale(1.3f);

  char buf[64];
  std::snprintf(buf, sizeof(buf), "FPS  %5.1f", fps);
  ImGui::Text("%s", buf);

  std::snprintf(buf, sizeof(buf), " ft  %5.2f ms", ft_last);
  ImGui::Text("%s", buf);

  if (nd >= 2) {
    std::snprintf(buf, sizeof(buf), "min  %5.2f ms", mn);
    ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "%s", buf);

    std::snprintf(buf, sizeof(buf), "avg  %5.2f ms", avg);
    ImGui::Text("%s", buf);

    std::snprintf(buf, sizeof(buf), "max  %5.2f ms", mx);
    ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.35f, 1.0f), "%s", buf);
  }

  ImGui::SetWindowFontScale(1.0f);
  ImGui::End();
}

}  // namespace ge
