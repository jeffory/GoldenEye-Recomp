// ge - ReXGlue Recompiled Project
//
// Guest-FPS benchmark recorder + overlay implementation. See ge_fps.h.
//
// SEMANTICS: every stat describes the guest's frame-production cadence between
// real submit-counter advances, measured with std::chrono::steady_clock (real
// wall-clock seconds; no guest-timebase ambiguity). The timestamp is taken
// BEFORE the frame limiter / present path, so this is "frames the game
// produced", not "frames the display showed" -- the two diverge when the
// present path drops frames. Corrections applied so the numbers match what a
// player would recognize:
//   - Coalesced advances (submit jumps by >1 between polls) are divided back
//     into per-frame times instead of recording one doubled "slow" frame.
//   - Load-screen / transition stalls (per-frame dt > ge_fps_gap_ms) are
//     counted separately as "gaps" and do NOT pollute avg/worst/1%-low.
//   - "hitch" counts accepted frames that took more than two of the title's
//     60Hz vblank slots (>41.7ms; a guest-engine constant, independent of the
//     panel rate; steady half-rate sections contribute zero).
//   - The old "best" (max fps) stat is gone: the shortest pre-throttle spacing
//     only measured catch-up bursts, which mean nothing to the player.
// All state is lock-free, fixed-size:
//   - count + sum_us           -> average FPS
//   - max_us (CAS)             -> worst single frame
//   - a frame-time histogram   -> 1%-low (FPS at the 99th-percentile frametime),
//                                 O(1) per frame, no allocation, no mutex
//   - an EMA of instantaneous FPS for a smooth live readout

#include "ge_fps.h"

#include <rex/cvar.h>
#include <rex/logging/macros.h>
#include <rex/perf/counter.h>

#include <imgui.h>

#include <atomic>
#include <chrono>
#include <cstdint>

// On-screen live readout (top-left). Default OFF on desktop (enable with
// --ge_fps_overlay=true); forced on for Android in GeApp::OnConfigurePaths.
REXCVAR_DEFINE_BOOL(ge_fps_overlay, false, "Debug",
                    "GoldenEye: draw an on-screen guest-FPS readout "
                    "(live / avg / 1%-low / worst / hitch+gap counts)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

// Periodic grep-able GEFPS summary line to ge.log (~1/s). Default OFF on desktop
// (enable with --ge_fps_log=true); forced on for Android in OnConfigurePaths.
REXCVAR_DEFINE_BOOL(ge_fps_log, false, "Debug",
                    "GoldenEye: log a periodic GEFPS avg/low1/worst/hitch/gap "
                    "summary line to ge.log")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

// On-screen overlay text scale (1.0 = ImGui default). Default 2x for readability
// at arm's length on a handheld; tunable live.
REXCVAR_DEFINE_DOUBLE(ge_fps_scale, 2.0, "Debug",
                      "GoldenEye: on-screen FPS overlay text scale (1.0 = default)")
    .range(0.5, 6.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

// Per-frame times above this are load screens / level transitions / FMV stalls,
// not gameplay frames: they are counted (and max-tracked) as "gaps" but excluded
// from avg / worst / 1%-low so one level load can't own the worst-frame stat.
REXCVAR_DEFINE_INT32(ge_fps_gap_ms, 250, "Debug",
                     "GoldenEye: frame times above this many ms count as load/"
                     "transition gaps instead of gameplay frames")
    .range(50, 2000)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

// Rate-limited GESPIKE ge.log line when a frame takes >2x the rolling median:
// the frame time plus the per-stage breakdown (CP execute/idle/fence, present
// block, guest GPU-wait, draws, stalls, CP-starvation episodes) so a spike can
// be attributed to a pipeline stage from the log alone. Default OFF on desktop;
// forced on for Android in GeApp::OnConfigurePaths.
REXCVAR_DEFINE_BOOL(ge_spike_log, false, "Debug",
                    "GoldenEye: log a GESPIKE stage-breakdown line when a frame "
                    "takes >2x the rolling median")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

// Present-cadence counters exported by the SDK presenter (presenter.cpp, same
// extern "C" pattern as rex_ge_cp_progress_seq). Monotonic; we take deltas.
extern "C" {
uint64_t rex_ge_present_paint_count();
uint64_t rex_ge_present_new_frame_count();
uint64_t rex_ge_guest_refresh_count();
uint64_t rex_ge_guest_drop_count();
uint64_t rex_ge_present_block_us();
#ifdef __ANDROID__
// Android-only (defined in the SDK's Android window/app-context backends).
uint64_t rex_ge_paint_request_count();
uint64_t rex_ge_ui_loop_iteration_count();
#endif
}

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
  std::atomic<uint64_t> count{0};           // frames this window (per-frame samples)
  std::atomic<uint64_t> sum_us{0};          // total frame time this window
  std::atomic<uint32_t> max_us{0};          // worst (longest) gameplay frame
  std::atomic<uint64_t> hitches{0};         // frames slower than 2x target period
  std::atomic<uint64_t> gaps{0};            // load/transition stalls (> ge_fps_gap_ms)
  std::atomic<uint64_t> max_gap_us{0};      // longest excluded gap
  std::atomic<uint32_t> median_us{0};       // rolling p50, refreshed at the 1s tick
  std::atomic<double> ema_fps{0.0};         // smoothed instantaneous FPS
  std::atomic<uint32_t> hist[kBins]{};

  void Reset() {
    last_frame_us.store(0, std::memory_order_relaxed);
    window_start_us.store(0, std::memory_order_relaxed);
    last_log_us.store(0, std::memory_order_relaxed);
    count.store(0, std::memory_order_relaxed);
    sum_us.store(0, std::memory_order_relaxed);
    max_us.store(0, std::memory_order_relaxed);
    hitches.store(0, std::memory_order_relaxed);
    gaps.store(0, std::memory_order_relaxed);
    max_gap_us.store(0, std::memory_order_relaxed);
    median_us.store(0, std::memory_order_relaxed);
    ema_fps.store(0.0, std::memory_order_relaxed);
    for (auto& b : hist) b.store(0, std::memory_order_relaxed);
  }
};

Accumulator& Acc() {
  static Accumulator a;
  return a;
}

template <typename T>
void CasMax(std::atomic<T>& a, T v) {
  T cur = a.load(std::memory_order_relaxed);
  while (v > cur && !a.compare_exchange_weak(cur, v, std::memory_order_relaxed)) {}
}

// Hitch = a frame that took MORE than two of the title's 60Hz vblank slots
// (2.5 x 16.67ms). The threshold is a guest-title constant, deliberately NOT
// derived from max_fps or the panel: GoldenEye's engine targets 60 regardless
// of the display (the Thor's 120Hz panel doesn't make it produce 120), max_fps
// is a *cap* the user can raise past the real rate (a desktop config with
// max_fps=120 shrank a derived threshold to 25ms and flagged every legitimate
// half-rate frame), and with vsync on max_fps is ignored anyway. Why 2.5x and
// not 2x/3x: the title runs whole sections at a steady half rate (2 slots --
// attract, menus), so 2x counts all of those; and vsync quantizes frame times
// to exact slot multiples, so a threshold AT the 3-slot time flips on
// microsecond jitter. 2.5x sits cleanly between the 2- and 3-slot buckets:
// steady 30-on-60 contributes zero, a third vblank always counts.
constexpr uint32_t kHitchThreshUs = 41'667;

struct Snapshot {
  double live, avg, low1, worst;  // FPS values
  uint64_t count, hitches, gaps;
  double max_gap_ms;
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
  const uint32_t mx = a.max_us.load(std::memory_order_relaxed);
  s.worst = (mx > 0) ? (1e6 / mx) : 0.0;
  s.hitches = a.hitches.load(std::memory_order_relaxed);
  s.gaps = a.gaps.load(std::memory_order_relaxed);
  s.max_gap_ms = a.max_gap_us.load(std::memory_order_relaxed) / 1000.0;

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

void FpsOnFrame(uint32_t frames_advanced) {
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

  // A poll can observe the submit counter jumping by >1 (the intermediate value
  // was never seen); the elapsed time then covers several produced frames. Split
  // it evenly instead of booking one doubled "slow" frame. Clamp defensively:
  // a huge jump means we lost track (fresh window, counter glitch), not that
  // dozens of frames really fit in this dt.
  if (frames_advanced < 1) frames_advanced = 1;
  if (frames_advanced > 4) frames_advanced = 4;
  const uint64_t per = (now - prev) / frames_advanced;

  // Sub-frame floor: the guest can emit non-display swaps (a clear + a present
  // on menu transitions) that land 1-3ms apart. This title never produces real
  // frames faster than ~63fps, so anything > 200fps is not a player frame.
  if (per < 5'000ull) return;

  // Load screens / level transitions / FMV stalls: track separately, keep them
  // out of the gameplay stats.
  const uint64_t gap_us =
      static_cast<uint64_t>(REXCVAR_GET(ge_fps_gap_ms)) * 1000ull;
  if (per > gap_us) {
    a.gaps.fetch_add(1, std::memory_order_relaxed);
    CasMax(a.max_gap_us, per);
    return;
  }

  if (per > kHitchThreshUs) {
    a.hitches.fetch_add(frames_advanced, std::memory_order_relaxed);
  }

  a.count.fetch_add(frames_advanced, std::memory_order_relaxed);
  a.sum_us.fetch_add(per * frames_advanced, std::memory_order_relaxed);
  CasMax(a.max_us, static_cast<uint32_t>(per));

  int bin = static_cast<int>((per / 1000.0) / kBinMs);
  if (bin < 0) bin = 0;
  if (bin >= kBins) bin = kBins - 1;
  a.hist[bin].fetch_add(frames_advanced, std::memory_order_relaxed);

  const double inst = 1e6 / static_cast<double>(per);
  const double ema = a.ema_fps.load(std::memory_order_relaxed);
  a.ema_fps.store(ema <= 0.0 ? inst : ema * 0.9 + inst * 0.1, std::memory_order_relaxed);

  // GESPIKE: a frame >2x the rolling median (and below the gap cutoff -- gaps
  // returned above) gets a rate-limited stage-breakdown line. The rex::perf
  // snapshot is the just-completed frame: the CP already executed this frame's
  // swap (Profiler::Flip ran) before the guest observed completion here.
  if (REXCVAR_GET(ge_spike_log)) {
    const uint32_t med = a.median_us.load(std::memory_order_relaxed);
    if (med > 0 && per > 2ull * med) {
      static std::atomic<uint64_t> s_last_spike_us{0};
      uint64_t last_spike = s_last_spike_us.load(std::memory_order_relaxed);
      if (now - last_spike >= 250'000ull &&
          s_last_spike_us.compare_exchange_strong(last_spike, now,
                                                  std::memory_order_relaxed)) {
        using rex::perf::CounterId;
        using rex::perf::GetSnapshotCounter;
        REXKRNL_INFO(
            "GESPIKE dt={:.1f}ms med={:.1f}ms cpexec={}us cpidle={}us "
            "wrm={}us present={}us gwait={}us gpu={}us draws={} stalls={} "
            "starved={}",
            per / 1000.0, med / 1000.0,
            GetSnapshotCounter(CounterId::kCpExecuteUs),
            GetSnapshotCounter(CounterId::kCpIdleUs),
            GetSnapshotCounter(CounterId::kCpWaitRegMemUs),
            GetSnapshotCounter(CounterId::kPresentBlockUs),
            GetSnapshotCounter(CounterId::kGuestGpuWaitUs),
            GetSnapshotCounter(CounterId::kGpuFrameUs),
            GetSnapshotCounter(CounterId::kDrawCalls),
            GetSnapshotCounter(CounterId::kCommandBufferStalls),
            GetCpStarvedEpisodes());
      }
    }
  }

  if (logit || REXCVAR_GET(ge_spike_log)) {
    const uint64_t last = a.last_log_us.load(std::memory_order_relaxed);
    if (now - last >= 1'000'000ull) {
      a.last_log_us.store(now, std::memory_order_relaxed);

      // Refresh the window median (p50 frametime since the last reset) from
      // the histogram for the spike detector. Same walk as 1%-low, different
      // target.
      const uint64_t n_med = a.count.load(std::memory_order_relaxed);
      if (n_med > 0) {
        const uint64_t target = (n_med + 1) / 2;
        uint64_t acc_n = 0;
        int bin = kBins - 1;
        for (int i = 0; i < kBins; ++i) {
          acc_n += a.hist[i].load(std::memory_order_relaxed);
          if (acc_n >= target) { bin = i; break; }
        }
        a.median_us.store(static_cast<uint32_t>((bin + 1) * kBinMs * 1000.0),
                          std::memory_order_relaxed);
      }

      if (!logit) return;
      Snapshot s = Read();
      REXKRNL_INFO(
          "GEFPS avg={:.1f} low1={:.1f} worst={:.1f} hitch={} gaps={} "
          "maxgap={:.0f}ms n={} dur={:.1f}s",
          s.avg, s.low1, s.worst, s.hitches, s.gaps, s.max_gap_ms, s.count,
          s.dur_s);

      // GESHOWN: where do produced frames go? Per-second deltas across the
      // present pipeline -- submit (this recorder) -> refresh (frames the CP
      // delivered to the present mailbox) -> new (paints that showed a new
      // frame) -> shown (successful presents); drop = mailbox frames replaced
      // before any paint consumed them (produced but never displayed). paint=
      // average PaintAndPresent wall time (the UI-thread block incl. FIFO
      // vsync wait). shown/new well below refresh + high drop = the paint
      // loop, not the game, is limiting displayed fps.
      // Benign single-writer statics: only the log-tick winner runs this.
      static uint64_t pv_t = 0, pv_paint = 0, pv_new = 0, pv_refresh = 0,
                      pv_drop = 0, pv_block = 0, pv_n = 0;
      const uint64_t c_paint = rex_ge_present_paint_count();
      const uint64_t c_new = rex_ge_present_new_frame_count();
      const uint64_t c_refresh = rex_ge_guest_refresh_count();
      const uint64_t c_drop = rex_ge_guest_drop_count();
      const uint64_t c_block = rex_ge_present_block_us();
      const uint64_t c_n = a.count.load(std::memory_order_relaxed);
      if (pv_t != 0 && now > pv_t) {
        const double secs = (now - pv_t) / 1e6;
        const uint64_t d_paint = c_paint - pv_paint;
        const double paint_ms =
            d_paint ? ((c_block - pv_block) / 1000.0 / d_paint) : 0.0;
        REXKRNL_INFO(
            "GESHOWN shown/s={:.1f} new/s={:.1f} refresh/s={:.1f} "
            "drop/s={:.1f} submit/s={:.1f} paint={:.2f}ms",
            d_paint / secs, (c_new - pv_new) / secs,
            (c_refresh - pv_refresh) / secs, (c_drop - pv_drop) / secs,
            (c_n - pv_n) / secs, paint_ms);
#ifdef __ANDROID__
        // req/s = paints requested (UI-loop wakes); loop/s = UI-loop
        // iterations. req/s < refresh/s means requests are being swallowed
        // upstream (presenter gate); loop/s < req/s means the loop can't keep
        // up with its wakes.
        static uint64_t pv_req = 0, pv_loop = 0;
        const uint64_t c_req = rex_ge_paint_request_count();
        const uint64_t c_loop = rex_ge_ui_loop_iteration_count();
        REXKRNL_INFO("GESHOWN2 req/s={:.1f} loop/s={:.1f}",
                     (c_req - pv_req) / secs, (c_loop - pv_loop) / secs);
        pv_req = c_req;
        pv_loop = c_loop;
#endif
      }
      pv_t = now;
      pv_paint = c_paint;
      pv_new = c_new;
      pv_refresh = c_refresh;
      pv_drop = c_drop;
      pv_block = c_block;
      pv_n = c_n;
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
    ImGui::Text("hitch %llu  gap %llu",
                static_cast<unsigned long long>(s.hitches),
                static_cast<unsigned long long>(s.gaps));
    ImGui::Text("n %llu  %.0fs", static_cast<unsigned long long>(s.count), s.dur_s);
  }
  ImGui::End();
}

}  // namespace ge
