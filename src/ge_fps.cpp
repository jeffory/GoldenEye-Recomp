// ge - ReXGlue Recompiled Project
//
// Guest-FPS benchmark recorder + overlay implementation. See ge_fps.h.
//
// The accumulator is fed once per real rendered frame (ge_dbg_now's submit-
// advance branch). It times frames with std::chrono::steady_clock -- real
// wall-clock seconds, so "FPS" means frames per real second and we avoid any
// guest-timebase frequency ambiguity. All state is lock-free, fixed-size:
//   - count + sum_us           -> average FPS
//   - min_us / max_us (CAS)    -> best / worst single frame
//   - a frame-time histogram   -> 1%-low (FPS at the 99th-percentile frametime),
//                                 O(1) per frame, no allocation, no mutex
//   - an EMA of instantaneous FPS for a smooth live readout

#include "ge_fps.h"

#include <rex/cvar.h>
#include <rex/logging/macros.h>

#include <imgui.h>

#include <atomic>
#include <chrono>
#include <cstdint>

// On-screen live readout (top-left). Default OFF on desktop (enable with
// --ge_fps_overlay=true); forced on for Android in GeApp::OnConfigurePaths.
REXCVAR_DEFINE_BOOL(ge_fps_overlay, false, "Debug",
                    "GoldenEye: draw an on-screen guest-FPS readout "
                    "(live / avg / 1%-low / worst / best)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

// Periodic grep-able GEFPS summary line to ge.log (~1/s). Default OFF on desktop
// (enable with --ge_fps_log=true); forced on for Android in OnConfigurePaths.
REXCVAR_DEFINE_BOOL(ge_fps_log, false, "Debug",
                    "GoldenEye: log a periodic GEFPS avg/low1/min/max summary line to ge.log")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

// On-screen overlay text scale (1.0 = ImGui default). Default 2x for readability
// at arm's length on a handheld; tunable live.
REXCVAR_DEFINE_DOUBLE(ge_fps_scale, 2.0, "Debug",
                      "GoldenEye: on-screen FPS overlay text scale (1.0 = default)")
    .range(0.5, 6.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace ge {
namespace {

// Histogram: 1000 bins x 0.1 ms = 0..100 ms (10 fps floor). Frames slower than
// 100 ms clamp into the last bin (still counted as a worst-case hitch).
constexpr int kBins = 1000;
constexpr double kBinMs = 0.1;

inline uint64_t NowUs() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

struct Accumulator {
  std::atomic<uint64_t> last_frame_us{0};   // timestamp of previous frame (0 = none)
  std::atomic<uint64_t> window_start_us{0}; // first frame of the current window
  std::atomic<uint64_t> last_log_us{0};     // last periodic-log emit
  std::atomic<uint64_t> count{0};           // frames this window (dt samples)
  std::atomic<uint64_t> sum_us{0};          // total frame time this window
  std::atomic<uint32_t> min_us{UINT32_MAX}; // best (shortest) frame
  std::atomic<uint32_t> max_us{0};          // worst (longest) frame
  std::atomic<double> ema_fps{0.0};         // smoothed instantaneous FPS
  std::atomic<uint32_t> hist[kBins]{};

  void Reset() {
    last_frame_us.store(0, std::memory_order_relaxed);
    window_start_us.store(0, std::memory_order_relaxed);
    last_log_us.store(0, std::memory_order_relaxed);
    count.store(0, std::memory_order_relaxed);
    sum_us.store(0, std::memory_order_relaxed);
    min_us.store(UINT32_MAX, std::memory_order_relaxed);
    max_us.store(0, std::memory_order_relaxed);
    ema_fps.store(0.0, std::memory_order_relaxed);
    for (auto& b : hist) b.store(0, std::memory_order_relaxed);
  }
};

Accumulator& Acc() {
  static Accumulator a;
  return a;
}

void CasMin(std::atomic<uint32_t>& a, uint32_t v) {
  uint32_t cur = a.load(std::memory_order_relaxed);
  while (v < cur && !a.compare_exchange_weak(cur, v, std::memory_order_relaxed)) {}
}
void CasMax(std::atomic<uint32_t>& a, uint32_t v) {
  uint32_t cur = a.load(std::memory_order_relaxed);
  while (v > cur && !a.compare_exchange_weak(cur, v, std::memory_order_relaxed)) {}
}

struct Snapshot {
  double live, avg, low1, worst, best;  // FPS values
  uint64_t count;
  double dur_s;
};

Snapshot Read() {
  Accumulator& a = Acc();
  Snapshot s{};
  const uint64_t n = a.count.load(std::memory_order_relaxed);
  const uint64_t sum = a.sum_us.load(std::memory_order_relaxed);
  s.count = n;
  s.live = a.ema_fps.load(std::memory_order_relaxed);
  s.avg = (sum > 0) ? (static_cast<double>(n) * 1e6 / static_cast<double>(sum)) : 0.0;
  const uint32_t mn = a.min_us.load(std::memory_order_relaxed);
  const uint32_t mx = a.max_us.load(std::memory_order_relaxed);
  s.best = (mn > 0 && mn != UINT32_MAX) ? (1e6 / mn) : 0.0;
  s.worst = (mx > 0) ? (1e6 / mx) : 0.0;

  // 1%-low: the FPS at the 99th-percentile frame time. Walk the histogram from
  // the fast end until cumulative count reaches 99% of samples; that bin's time
  // is the frametime 99% of frames beat -> 1/it is the 1%-low FPS.
  if (n > 0) {
    const uint64_t target = (n * 99 + 99) / 100;  // ceil(0.99 * n)
    uint64_t acc = 0;
    int bin = kBins - 1;
    for (int i = 0; i < kBins; ++i) {
      acc += a.hist[i].load(std::memory_order_relaxed);
      if (acc >= target) { bin = i; break; }
    }
    const double ms = (bin + 1) * kBinMs;  // upper edge of the bin
    s.low1 = (ms > 0.0) ? (1000.0 / ms) : 0.0;
  }

  const uint64_t start = a.window_start_us.load(std::memory_order_relaxed);
  s.dur_s = (start > 0) ? (NowUs() - start) / 1e6 : 0.0;
  return s;
}

}  // namespace

void FpsReset() { Acc().Reset(); }

void FpsOnFrame() {
  const bool overlay = REXCVAR_GET(ge_fps_overlay);
  const bool logit = REXCVAR_GET(ge_fps_log);
  if (!overlay && !logit) {
    // Recorder is off: keep last_frame_us fresh so the first dt after enabling
    // isn't an inflated gap, but skip all accounting.
    Acc().last_frame_us.store(NowUs(), std::memory_order_relaxed);
    return;
  }

  Accumulator& a = Acc();
  const uint64_t now = NowUs();
  const uint64_t prev = a.last_frame_us.exchange(now, std::memory_order_relaxed);
  uint64_t exp0 = 0;
  a.window_start_us.compare_exchange_strong(exp0, now, std::memory_order_relaxed);
  if (prev == 0 || now <= prev) return;  // first frame of window / clock noise

  uint64_t dt = now - prev;
  // Reject non-displayed frames. The guest submit counter we tick on can advance
  // more than once per visible frame (a clear + a present, menu transitions), so
  // some "frames" land 1-3ms apart -- not distinct frames the player sees, and
  // they'd corrupt the best-frame metric / inflate the count. This title never
  // displays faster than ~63 fps, so treat anything > 200 fps (dt < 5ms) as a
  // sub-frame swap and drop it. dt > 2s is a load screen / pause gap.
  if (dt < 5'000ull || dt > 2'000'000ull) return;

  a.count.fetch_add(1, std::memory_order_relaxed);
  a.sum_us.fetch_add(dt, std::memory_order_relaxed);
  CasMin(a.min_us, static_cast<uint32_t>(dt));
  CasMax(a.max_us, static_cast<uint32_t>(dt));

  int bin = static_cast<int>((dt / 1000.0) / kBinMs);
  if (bin < 0) bin = 0;
  if (bin >= kBins) bin = kBins - 1;
  a.hist[bin].fetch_add(1, std::memory_order_relaxed);

  const double inst = 1e6 / static_cast<double>(dt);
  const double ema = a.ema_fps.load(std::memory_order_relaxed);
  a.ema_fps.store(ema <= 0.0 ? inst : ema * 0.9 + inst * 0.1, std::memory_order_relaxed);

  if (logit) {
    const uint64_t last = a.last_log_us.load(std::memory_order_relaxed);
    if (now - last >= 1'000'000ull) {
      a.last_log_us.store(now, std::memory_order_relaxed);
      Snapshot s = Read();
      REXKRNL_INFO("GEFPS avg={:.1f} low1={:.1f} min={:.1f} max={:.1f} n={} dur={:.1f}s",
                   s.avg, s.low1, s.worst, s.best, s.count, s.dur_s);
    }
  }
}

FpsOverlay::FpsOverlay(rex::ui::ImGuiDrawer* drawer) : rex::ui::ImGuiDialog(drawer) {}
FpsOverlay::~FpsOverlay() = default;

void FpsOverlay::OnDraw(ImGuiIO& io) {
  if (!REXCVAR_GET(ge_fps_overlay)) return;
  (void)io;

  Snapshot s = Read();
  ImGui::SetNextWindowPos(ImVec2(8, 8), ImGuiCond_Always);
  ImGui::SetNextWindowBgAlpha(0.55f);
  ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                           ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                           ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoInputs |
                           ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing |
                           ImGuiWindowFlags_AlwaysAutoResize;
  if (ImGui::Begin("##ge_fps", nullptr, flags)) {
    ImGui::SetWindowFontScale(  // overlay text scale (default 2x); AlwaysAutoResize grows to fit
        static_cast<float>(REXCVAR_GET(ge_fps_scale)));
    ImGui::Text("FPS %5.1f", s.live);
    ImGui::Separator();
    ImGui::Text("avg   %5.1f", s.avg);
    ImGui::Text("1%%low %5.1f", s.low1);
    ImGui::Text("worst %5.1f", s.worst);
    ImGui::Text("best  %5.1f", s.best);
    ImGui::Text("n %llu  %.0fs", static_cast<unsigned long long>(s.count), s.dur_s);
  }
  ImGui::End();
}

}  // namespace ge
