// ge - input record/replay/bench harness. See ge_replay.h.
//
// This file is yours to edit. 'rexglue migrate' will NOT overwrite it.
//
// Task 2 delivers the skeleton (cvars + the input-override registration) plus
// a poll-cadence probe: ge_replay_probe logs, once a second, how many times
// the guest polled gamepad state (rex::input::SetGetStateOverride fires once
// per XInputGetState call) per real guest frame produced. That ratio gates
// the recording format for Tasks 3+ -- a steady polls_per_frame means "record
// once per N polls, replay by re-emitting the same state N times" is safe; a
// variable ratio means records need an explicit per-record frame index
// instead (see the task brief's documented fallback).
//
// Task 5 lands the replayer: a kIdle -> kMacro -> kWaitLevel -> kPlaying ->
// kDone state machine driving the same X_INPUT_GAMEPAD boundary as the
// recorder, plus the GEBENCH summary line and ge_bench_exit auto-quit. With
// the harness cvars at their defaults (nothing to play, nothing to record),
// ReplayOnGetState still has no gameplay effect.

#include "ge_replay.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/perf/counter.h>
#include <rex/input/input.h>
#include <rex/input/input_system.h>

#include "ge_hooks.h"

REXCVAR_DEFINE_STRING(ge_replay_record, "", "GE",
                      "Record guest gamepad input to this file (starts at level entry)");
REXCVAR_DEFINE_STRING(ge_replay_play, "", "GE",
                      "Replay guest gamepad input from this file");
REXCVAR_DEFINE_STRING(ge_replay_macro, "", "GE",
                      "Menu macro to run before the replay (flag-waiting button script)");
REXCVAR_DEFINE_BOOL(ge_replay_probe, false, "GE",
                    "Log GEREPLAY PROBE poll-cadence lines once per second");
REXCVAR_DEFINE_BOOL(ge_replay_selftest, false, "GE",
                    "Run the record->replay loop-back self-test at startup and log the verdict");
REXCVAR_DEFINE_BOOL(ge_bench_exit, false, "GE",
                    "Quit the app after the replay finishes and GEBENCH is emitted");

// Guest frame-production counter for the probe's polls_per_frame denominator.
//
// The brief called for `rex_ge_present_submit_count()` "same extern the FPS
// recorder uses" -- but ge_fps.cpp (lines 93-104) does not declare or use any
// function by that name; the FPS recorder's own "submit" stat (the GESHOWN
// log line's submit/s column) is fed by ge_hooks.cpp's ge_dbg_now reading a
// guest-memory submit counter directly (dev+16544, never exported via
// extern "C"), not by a queryable SDK function. Of the extern "C" counters
// ge_fps.cpp DOES declare and use (presenter.cpp, same rex_ge_cp_progress_seq
// pattern), rex_ge_guest_refresh_count() is the correct substitute: presenter.cpp
// documents it as "guest frames delivered to the mailbox" -- i.e. the guest
// SUBMITTING a frame into the present pipeline, before any host-side paint/
// display-rate limiting (paint/new/shown count) can throw frames away. Using
// paint_count instead would conflate "frames the guest produced" with "frames
// the UI thread painted", which is exactly the gap this SDK counter was added
// to measure (see presenter.cpp's "how many guest frames actually reach the
// display" comment) and would corrupt the polls_per_frame ratio if the host
// ever drops presents.
//
// Task 5 reuses the same counter as BenchOnPoll's submit-gate: the brief
// specified `rex_ge_present_submit_count()`, which does not exist anywhere in
// the SDK (grepped the whole tree) -- this is the same substitution as above,
// for the same reason.
extern "C" uint64_t rex_ge_guest_refresh_count();

namespace ge {
namespace {

#pragma pack(push, 1)
struct ReplayHeader {
  uint32_t magic;         // 'GERP' = 0x50524547 little-endian
  uint32_t version;       // 1
  uint32_t poll_hz_hint;  // informational (from the probe, e.g. 60)
  uint32_t reserved;      // 0
  uint64_t level_flag_addr;  // informational: guest global GeInLevel() anchors
                             // on, 0x82F1E704 (current stage number; task-6)
  uint32_t record_count;     // patched on Close(); 0 = read to EOF
};
#pragma pack(pop)
static_assert(sizeof(ReplayHeader) == 28, "replay header layout");
constexpr uint32_t kReplayMagic = 0x50524547u;

class Recorder {
 public:
  bool Open(const std::string& path) {
    file_ = std::fopen(path.c_str(), "wb");
    if (!file_) {
      REXKRNL_WARN("GEREPLAY cannot open '{}' for recording", path);
      return false;
    }
    ReplayHeader h{kReplayMagic, 1, 60, 0, 0x82F1E704ull, 0};
    std::fwrite(&h, sizeof(h), 1, file_);
    count_ = 0;
    return true;
  }
  void OnPoll(const rex::input::X_INPUT_STATE& state) {
    if (!file_) return;
    std::fwrite(&state.gamepad, sizeof(rex::input::X_INPUT_GAMEPAD), 1, file_);  // raw guest-endian
    if ((++count_ & 63) == 0) std::fflush(file_);
  }
  void Close() {
    if (!file_) return;
    std::fseek(file_, offsetof(ReplayHeader, record_count), SEEK_SET);
    std::fwrite(&count_, sizeof(count_), 1, file_);
    std::fclose(file_);
    file_ = nullptr;
    REXKRNL_INFO("GEREPLAY recorded {} polls", count_);
  }
  uint32_t count() const { return count_; }

 private:
  std::FILE* file_ = nullptr;
  uint32_t count_ = 0;
};

class Player {
 public:
  bool Open(const std::string& path) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
      REXKRNL_WARN("GEREPLAY cannot open '{}' for replay", path);
      return false;
    }
    ReplayHeader h{};
    if (std::fread(&h, sizeof(h), 1, f) != 1 || h.magic != kReplayMagic || h.version != 1) {
      REXKRNL_WARN("GEREPLAY '{}' is not a v1 recording", path);
      std::fclose(f);
      return false;
    }
    rex::input::X_INPUT_GAMEPAD pad;
    while (std::fread(&pad, sizeof(pad), 1, f) == 1) {
      records_.push_back(pad);
      if (h.record_count && records_.size() >= h.record_count) break;
    }
    std::fclose(f);
    next_ = 0;
    REXKRNL_INFO("GEREPLAY loaded {} polls from '{}'", records_.size(), path);
    return !records_.empty();
  }
  bool Next(rex::input::X_INPUT_GAMEPAD* out) {
    if (next_ >= records_.size()) return false;
    *out = records_[next_++];
    return true;
  }
  size_t remaining() const { return records_.size() - next_; }

 private:
  std::vector<rex::input::X_INPUT_GAMEPAD> records_;
  size_t next_ = 0;
};

// --- menu macro ------------------------------------------------------------
//
// Button-name -> XINPUT bit table. Values are the guest-facing
// X_INPUT_GAMEPAD_* constants from rex/input/input.h (host-order bit masks --
// see the be<> note on Macro::OnPoll below). LS/RS are the thumbstick click
// buttons, LB/RB are the shoulder buttons.
struct ButtonName {
  const char* name;
  uint16_t value;
};
constexpr ButtonName kButtonNames[] = {
    {"DPAD_UP", rex::input::X_INPUT_GAMEPAD_DPAD_UP},
    {"DPAD_DOWN", rex::input::X_INPUT_GAMEPAD_DPAD_DOWN},
    {"DPAD_LEFT", rex::input::X_INPUT_GAMEPAD_DPAD_LEFT},
    {"DPAD_RIGHT", rex::input::X_INPUT_GAMEPAD_DPAD_RIGHT},
    {"START", rex::input::X_INPUT_GAMEPAD_START},
    {"BACK", rex::input::X_INPUT_GAMEPAD_BACK},
    {"LS", rex::input::X_INPUT_GAMEPAD_LEFT_THUMB},
    {"RS", rex::input::X_INPUT_GAMEPAD_RIGHT_THUMB},
    {"LB", rex::input::X_INPUT_GAMEPAD_LEFT_SHOULDER},
    {"RB", rex::input::X_INPUT_GAMEPAD_RIGHT_SHOULDER},
    {"A", rex::input::X_INPUT_GAMEPAD_A},
    {"B", rex::input::X_INPUT_GAMEPAD_B},
    {"X", rex::input::X_INPUT_GAMEPAD_X},
    {"Y", rex::input::X_INPUT_GAMEPAD_Y},
};

bool ButtonByName(const std::string& name, uint16_t* out) {
  for (const auto& b : kButtonNames) {
    if (name == b.name) {
      *out = b.value;
      return true;
    }
  }
  return false;
}

bool AxisByName(const std::string& name, int* out) {
  if (name == "LX") { *out = 0; return true; }
  if (name == "LY") { *out = 1; return true; }
  if (name == "RX") { *out = 2; return true; }
  if (name == "RY") { *out = 3; return true; }
  return false;
}

struct MacroStep {
  enum Kind { kPress, kStick, kWaitPolls, kWaitFlag } kind;
  uint16_t button = 0;      // kPress
  int axis = 0;             // kStick: 0=LX 1=LY 2=RX 3=RY
  int16_t axis_value = 0;   // kStick
  uint32_t frames = 2;      // kPress/kStick hold, kWaitPolls count
  uint32_t flag_value = 0;  // kWaitFlag: expected GeInLevel()?1:0
  uint32_t timeout = 36000; // kWaitFlag: polls (~10 min) before giving up
};

// Flag-waiting menu-macro engine. Drives a scripted button sequence (see
// bench/dam.macro) through Recorder/Player's same X_INPUT_GAMEPAD boundary,
// so a macro's output is indistinguishable from a real pad press to the guest
// and to anything else that consumes ReplayOnGetState.
//
// be<> semantics (confirmed from rex/types.h): `be<T>` has a non-explicit
// converting constructor `endian_store(const T& src)` that calls set(), which
// byte-swaps a host-order value into big-endian storage on a little-endian
// host (native != E). The implicitly-defaulted copy-assignment operator only
// copies the already-swapped `value` member with no further swap. So
// `out->buttons = <host-order value>` goes through exactly one swap (via the
// converting ctor + defaulted assign) -- plain assignment of a host-order
// value is correct and matches the SDK's own input drivers, e.g.
// xinput_input_driver.cpp: `out_state->gamepad.buttons =
// native_state.state.Gamepad.wButtons;`. Do NOT pre-swap with
// rex::byte_swap() before assigning -- that would double-swap.
class Macro {
 public:
  // Parses `path` into steps_. Logs (REXKRNL_ERROR) and returns false on any
  // syntax error; on failure, any previously-loaded macro state is left
  // untouched (the new steps are only committed on full success).
  bool Load(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) {
      REXKRNL_ERROR("GEREPLAY macro cannot open '{}'", path);
      return false;
    }

    // Parses a decimal integer within [min,max]; returns false on any junk,
    // sign, or range violation so a typo fails the load instead of wrapping.
    auto parse_int = [](const std::string& tok, int64_t min, int64_t max, int64_t* out) {
      if (tok.empty()) return false;
      size_t i = (tok[0] == '-') ? 1u : 0u;
      if (i >= tok.size()) return false;
      for (size_t j = i; j < tok.size(); ++j) {
        if (tok[j] < '0' || tok[j] > '9') return false;
      }
      errno = 0;
      long long v = std::strtoll(tok.c_str(), nullptr, 10);
      if (errno == ERANGE || v < min || v > max) return false;
      *out = v;
      return true;
    };

    std::vector<MacroStep> steps;
    std::string line;
    uint32_t line_no = 0;
    while (std::getline(f, line)) {
      ++line_no;
      const auto hash = line.find('#');
      if (hash != std::string::npos) line.resize(hash);
      std::istringstream iss(line);
      std::vector<std::string> tok;
      for (std::string t; iss >> t;) tok.push_back(std::move(t));
      if (tok.empty()) continue;  // blank or comment-only line

      MacroStep step;
      bool ok = true;
      try {
        if (tok[0] == "press") {
          step.kind = MacroStep::kPress;
          ok = tok.size() >= 2 && tok.size() <= 3 && ButtonByName(tok[1], &step.button);
          if (ok && tok.size() == 3) {
            int64_t frames = 0;
            ok = parse_int(tok[2], 1, 100000, &frames);
            if (ok) step.frames = static_cast<uint32_t>(frames);
          } else if (ok) {
            step.frames = 2;
          }
        } else if (tok[0] == "stick") {
          step.kind = MacroStep::kStick;
          ok = tok.size() == 4 && AxisByName(tok[1], &step.axis);
          if (ok) {
            int64_t axis_val = 0;
            int64_t frames = 0;
            ok = parse_int(tok[2], -32768, 32767, &axis_val) &&
                 parse_int(tok[3], 1, 100000, &frames);
            if (ok) {
              step.axis_value = static_cast<int16_t>(axis_val);
              step.frames = static_cast<uint32_t>(frames);
            }
          }
        } else if (tok[0] == "wait_polls") {
          step.kind = MacroStep::kWaitPolls;
          ok = tok.size() == 2;
          if (ok) {
            int64_t frames = 0;
            ok = parse_int(tok[1], 1, 10000000, &frames);
            if (ok) step.frames = static_cast<uint32_t>(frames);
          }
        } else if (tok[0] == "wait_flag") {
          step.kind = MacroStep::kWaitFlag;
          ok = tok.size() >= 3 && tok.size() <= 4 && tok[1] == "in_level";
          if (ok) {
            int64_t flag_val = 0;
            int64_t timeout = 36000;
            ok = parse_int(tok[2], 0, 1, &flag_val);
            if (ok && tok.size() == 4) {
              ok = parse_int(tok[3], 1, 10000000, &timeout);
            }
            if (ok) {
              step.flag_value = static_cast<uint32_t>(flag_val);
              step.timeout = static_cast<uint32_t>(timeout);
            }
          }
        } else {
          ok = false;  // unknown command token
        }
      } catch (const std::exception&) {
        ok = false;  // malformed number
      }
      if (!ok) {
        REXKRNL_ERROR("GEREPLAY macro parse error at line {}", line_no);
        return false;
      }
      steps.push_back(step);
    }
    steps_ = std::move(steps);
    step_ = 0;
    step_polls_ = 0;
    failed_ = false;
    return true;
  }

  bool Done() const { return step_ >= steps_.size() && !failed_; }
  bool Failed() const { return failed_; }
  size_t StepCount() const { return steps_.size(); }

  // Zeros *out, then applies the current step's effect (if any) and advances
  // the step machine. kPress/kStick hold their effect for `frames` polls, then
  // emit a 2-poll neutral gap before the next step starts, so two consecutive
  // presses of the same button register as distinct edges to the guest.
  void OnPoll(rex::input::X_INPUT_GAMEPAD* out) {
    std::memset(out, 0, sizeof(*out));
    if (Failed() || Done()) return;

    MacroStep& step = steps_[step_];
    switch (step.kind) {
      case MacroStep::kPress:
      case MacroStep::kStick: {
        if (step_polls_ < step.frames) {
          if (step.kind == MacroStep::kPress) {
            out->buttons = step.button;  // see be<> note above
          } else {
            switch (step.axis) {
              case 0: out->thumb_lx = step.axis_value; break;
              case 1: out->thumb_ly = step.axis_value; break;
              case 2: out->thumb_rx = step.axis_value; break;
              case 3: out->thumb_ry = step.axis_value; break;
              default: break;
            }
          }
        }
        // else: neutral gap poll -- *out is already zeroed above.
        if (++step_polls_ >= step.frames + 2) {
          step_polls_ = 0;
          REXKRNL_INFO("GEREPLAY macro step {} ({}) done", step_,
                       step.kind == MacroStep::kPress ? "press" : "stick");
          ++step_;
        }
        break;
      }
      case MacroStep::kWaitPolls: {
        if (++step_polls_ >= step.frames) {
          step_polls_ = 0;
          REXKRNL_INFO("GEREPLAY macro step {} (wait_polls) done", step_);
          ++step_;
        }
        break;
      }
      case MacroStep::kWaitFlag: {
        const bool want = step.flag_value == 1;
        if (GeInLevel() == want) {
          step_polls_ = 0;
          REXKRNL_INFO("GEREPLAY macro step {} (wait_flag) satisfied", step_);
          ++step_;
        } else if (step.timeout == 0) {
          failed_ = true;
          REXKRNL_ERROR("GEREPLAY macro timeout at step {}", step_);
        } else {
          --step.timeout;
        }
        break;
      }
    }
  }

 private:
  std::vector<MacroStep> steps_;
  size_t step_ = 0;
  uint32_t step_polls_ = 0;
  bool failed_ = false;
};

std::function<void()> g_quit_requester;

// Real (nonzero) pad-state test shared by the recorder's menu-input gate and
// its dossier-dismiss detection below. Sticks need real deflection (~25%) so
// ADC drift on an idle pad can't arm anything.
bool IsNonzeroGamepadState(const rex::input::X_INPUT_GAMEPAD& gp) {
  return gp.buttons || gp.left_trigger || gp.right_trigger ||
         std::abs(int(gp.thumb_lx)) > 8000 || std::abs(int(gp.thumb_ly)) > 8000 ||
         std::abs(int(gp.thumb_rx)) > 8000 || std::abs(int(gp.thumb_ry)) > 8000;
}

// --- recording state -------------------------------------------------------
Recorder g_recorder;
std::atomic<bool> g_recording{false};
bool g_record_armed = false;
std::string g_record_path;
bool g_saw_menu_input = false;
// Set once, after in_level && g_saw_menu_input, by the first real pad state
// seen while in_level -- the player's mission-dossier dismiss press. The
// recorder arms on the next all-neutral poll after that (see the Recording
// control block below), using the input-gated dossier as a load-time-
// invariant sync barrier instead of in_level's edge directly.
bool g_saw_level_input = false;

// --- replay state machine ---------------------------------------------------
// kIdle -> kMacro (only if a macro is loaded) -> kWaitLevel -> kPlaying -> kDone.
// Record and play are mutually exclusive (ReplayInit refuses to arm this
// machine -- leaving it at kIdle -- if recording is also armed), so whenever
// g_state != kIdle, ReplayOnGetState's switch returns before it can reach the
// recorder branch: a replaying run never also records.
enum ReplayState { kIdle, kMacro, kWaitLevel, kPlaying, kDone };
Player g_player;
Macro g_macro;
ReplayState g_state = kIdle;
// A loaded macro reaching Done() counts as evidence of real menu navigation
// for the kWaitLevel gate below, same as g_saw_menu_input -- a scripted
// macro drives synthesized pad state, so it never sets g_saw_menu_input
// itself (that tracker only looks at pre-substitution, i.e. real, input).
bool g_macro_ran = false;
// Synthesized X_INPUT_STATE::packet_number, monotonic across every poll the
// state machine substitutes (kMacro and kPlaying both write it).
uint32_t g_packet = 0;

// --- bench (GEBENCH) ---------------------------------------------------------
struct BenchState {
  std::vector<int64_t> poll_ts_us;
  // Per-frame samples, not running totals: the attribution question (see
  // docs/superpowers/specs/2026-08-05-phase0-dam-gpu-attribution-design.md)
  // compares a *median* GPU frame time against a 13.5ms fixed cost, and a mean
  // would be dragged by the very hitches the analysis excludes.
  std::vector<int64_t> gpu_us;
  std::vector<int64_t> draws;
  int64_t strans_us = 0;
  int64_t pcomp_us = 0;
  int64_t texup_us = 0;
  int64_t gio_us = 0;
  uint64_t last_refresh = 0;  // rex_ge_guest_refresh_count() as of the last poll
};
BenchState g_bench;

void BenchStart() {
  g_bench = BenchState{};
  g_bench.last_refresh = rex_ge_guest_refresh_count();
}

// Called once per substituted playback poll. Always records a poll timestamp
// (frame-time percentiles are derived from poll-to-poll deltas); only adds
// the four stage totals when the guest-refresh-count has advanced since the
// last poll, so a guest that polls more than once per frame (see the Task-2
// probe) can't have these per-frame accumulators double-counted into the
// running totals.
void BenchOnPoll() {
  int64_t us = std::chrono::duration_cast<std::chrono::microseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
                   .count();
  g_bench.poll_ts_us.push_back(us);

  uint64_t refresh = rex_ge_guest_refresh_count();
  if (refresh != g_bench.last_refresh) {
    g_bench.last_refresh = refresh;
    g_bench.strans_us += rex::perf::GetSnapshotCounter(rex::perf::CounterId::kShaderTranslateUs);
    g_bench.pcomp_us += rex::perf::GetSnapshotCounter(rex::perf::CounterId::kPipelineCompileUs);
    g_bench.texup_us += rex::perf::GetSnapshotCounter(rex::perf::CounterId::kTextureUploadUs);
    g_bench.gio_us += rex::perf::GetSnapshotCounter(rex::perf::CounterId::kGuestFileIoUs);
    g_bench.gpu_us.push_back(
        rex::perf::GetSnapshotCounter(rex::perf::CounterId::kGpuFrameUs));
    g_bench.draws.push_back(
        rex::perf::GetSnapshotCounter(rex::perf::CounterId::kDrawCalls));
  }
}

void BenchFinish() {
  const auto& ts = g_bench.poll_ts_us;
  if (ts.size() < 2) { REXKRNL_WARN("GEBENCH too short"); return; }
  std::vector<int64_t> ft;
  ft.reserve(ts.size() - 1);
  for (size_t i = 1; i < ts.size(); ++i) ft.push_back(ts[i] - ts[i - 1]);
  std::sort(ft.begin(), ft.end());
  // avg is the fps of the *median* poll interval (not a mean of per-poll fps
  // samples) -- same basis as low1/worst, which are also interval percentiles.
  // Per the Task-2 probe, polls_per_frame is steady (~1) on both desktop and
  // arm64, so a poll interval approximates a sim-frame interval.
  auto fps = [](int64_t us) { return us > 0 ? 1e6 / double(us) : 0.0; };
  double dur_s = double(ts.back() - ts.front()) / 1e6;
  int64_t p50 = ft[ft.size() / 2];
  int64_t p99 = ft[size_t(double(ft.size() - 1) * 0.99)];
  int64_t worst = ft.back();
  size_t hitches = size_t(std::count_if(ft.begin(), ft.end(),
                                        [](int64_t us) { return us > 41667; }));
  // Percentile over a copy (sorts in place). p is 0..100.
  auto pctl = [](std::vector<int64_t> v, int p) -> double {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    size_t idx = size_t(double(v.size() - 1) * (double(p) / 100.0));
    return double(v[idx]);
  };
  const double gpu_med_ms = pctl(g_bench.gpu_us, 50) / 1000.0;
  const double gpu_p95_ms = pctl(g_bench.gpu_us, 95) / 1000.0;
  const double draws_med = pctl(g_bench.draws, 50);
  // gpu_med_ms can be built from far fewer samples than `frames=` suggests --
  // samples are only appended when the guest refresh advances (BenchOnPoll's
  // guard), and the SDK's GPU-timestamp slots can themselves skip. Surface
  // the sample count and how many came back zero so a reader can sanity-check
  // the estimator instead of trusting frames= as a proxy for it. Appended at
  // the end of the format string (not inserted) because perf_report.py's
  // GEBENCH_RE is unanchored -- appending is safe, inserting would break it.
  const size_t gpu_n = g_bench.gpu_us.size();
  const size_t gpu_zero = size_t(
      std::count(g_bench.gpu_us.begin(), g_bench.gpu_us.end(), int64_t{0}));
  REXKRNL_INFO(
      "GEBENCH frames={} dur={:.1f}s avg={:.1f} low1={:.1f} worst={:.1f} hitch={} "
      "gpu_med_ms={:.2f} gpu_p95_ms={:.2f} draws_med={:.0f} "
      "strans_ms={:.1f} pcomp_ms={:.1f} texup_ms={:.1f} gio_ms={:.1f} "
      "gpu_n={} gpu_zero={}",
      ft.size(), dur_s, fps(p50), fps(p99), fps(worst), hitches,
      gpu_med_ms, gpu_p95_ms, draws_med,
      g_bench.strans_us / 1000.0, g_bench.pcomp_us / 1000.0, g_bench.texup_us / 1000.0,
      g_bench.gio_us / 1000.0, gpu_n, gpu_zero);
  if (REXCVAR_GET(ge_bench_exit) && g_quit_requester) {
    g_quit_requester();
  }
}

// --- self-test -------------------------------------------------------------
void RunSelfTest() {
  const std::string path =
      (std::filesystem::temp_directory_path() / "ge_replay_selftest.bin").string();
  Recorder rec;
  if (!rec.Open(path)) {
    REXKRNL_ERROR("GEREPLAY SELFTEST FAIL (open)");
    return;
  }
  std::vector<rex::input::X_INPUT_STATE> sent(100);
  for (uint32_t i = 0; i < 100; ++i) {
    sent[i].gamepad.buttons = uint16_t(i * 7 + 1);
    sent[i].gamepad.left_trigger = uint8_t(i);
    sent[i].gamepad.right_trigger = uint8_t(255 - i);
    sent[i].gamepad.thumb_lx = int16_t(i * 301 - 15000);
    sent[i].gamepad.thumb_ly = int16_t(15000 - i * 301);
    sent[i].gamepad.thumb_rx = int16_t(i * 17);
    sent[i].gamepad.thumb_ry = int16_t(-int(i) * 17);
    rec.OnPoll(sent[i]);
  }
  rec.Close();
  Player play;
  if (!play.Open(path) || play.remaining() != 100) {
    REXKRNL_ERROR("GEREPLAY SELFTEST FAIL (load, {} records)", play.remaining());
    return;
  }
  for (uint32_t i = 0; i < 100; ++i) {
    rex::input::X_INPUT_GAMEPAD pad{};
    if (!play.Next(&pad) || std::memcmp(&pad, &sent[i].gamepad, sizeof(pad)) != 0) {
      REXKRNL_ERROR("GEREPLAY SELFTEST FAIL (mismatch at {})", i);
      return;
    }
  }
  REXKRNL_INFO("GEREPLAY SELFTEST PASS (100 records round-tripped)");

  // Macro parser syntax-check (Step 3): the macro engine itself is exercised
  // end-to-end in later tasks; here we just confirm bench/dam.macro parses.
  // cwd-relative path -- works when the binary is run from the repo root (see
  // the build/run recipe); skip silently if it isn't there.
  const char* kMacroPath = "bench/dam.macro";
  if (std::filesystem::exists(kMacroPath)) {
    Macro macro;
    if (macro.Load(kMacroPath)) {
      REXKRNL_INFO("GEREPLAY SELFTEST macro parse OK ({} steps)", macro.StepCount());
    } else {
      REXKRNL_ERROR("GEREPLAY SELFTEST macro parse FAIL");
    }
  }
}

// --- probe -------------------------------------------------------------
struct ProbeState {
  std::atomic<uint64_t> polls{0};
  uint64_t last_polls = 0;
  uint64_t last_submits = 0;
  std::chrono::steady_clock::time_point last_log{};
};
ProbeState g_probe;

void ProbeOnPoll() {
  uint64_t n = g_probe.polls.fetch_add(1, std::memory_order_relaxed) + 1;
  auto now = std::chrono::steady_clock::now();
  if (now - g_probe.last_log < std::chrono::seconds(1)) {
    return;
  }
  g_probe.last_log = now;
  uint64_t submits = rex_ge_guest_refresh_count();
  uint64_t dp = n - g_probe.last_polls;
  uint64_t ds = submits - g_probe.last_submits;
  g_probe.last_polls = n;
  g_probe.last_submits = submits;
  REXKRNL_INFO("GEREPLAY PROBE polls/s={} submits/s={} polls_per_frame={:.2f} in_level={} "
               "flag={:08X} player={:08X} stage={:08X}",
               dp, ds, ds ? double(dp) / double(ds) : 0.0, GeInLevel() ? 1 : 0,
               GeDbgLevelFlag(), GeDbgPlayerPtr(), GeDbgStageNum());
}

bool ReplayOnGetState(uint32_t user_index, rex::input::X_INPUT_STATE* state) {
  if (user_index != 0) {
    return false;
  }
  if (REXCVAR_GET(ge_replay_probe)) {
    ProbeOnPoll();
  }

  const bool in_level = GeInLevel();

  // Track menu input to gate recording (Attract mode sets the in-level flag too;
  // require real menu input first so an idle title screen can't start the recording.)
  if (!in_level && IsNonzeroGamepadState(state->gamepad)) {
    g_saw_menu_input = true;
  }

  // Replay state machine. Each active state (kMacro/kWaitLevel/kPlaying)
  // returns directly from this switch, so whenever a replay is armed the
  // recorder branch below is unreachable this poll -- see the g_state comment
  // at its declaration for why that's the record/play mutual exclusion.
  switch (g_state) {
    case kMacro: {
      rex::input::X_INPUT_GAMEPAD pad{};
      g_macro.OnPoll(&pad);
      state->gamepad = pad;
      state->packet_number = ++g_packet;
      if (g_macro.Failed()) { g_state = kDone; break; }
      if (g_macro.Done()) {
        g_macro_ran = true;
        g_state = kWaitLevel;
      }
      return true;
    }
    case kWaitLevel:
      // Attract mode sets the in-level flag too (same as the recorder's
      // guard above); require real menu input OR a completed macro as
      // evidence of actual navigation before treating this as playback start.
      if (in_level && (g_saw_menu_input || g_macro_ran)) {
        g_state = kPlaying;
        BenchStart();
        REXKRNL_INFO("GEREPLAY playback started ({} polls)", g_player.remaining());
      }
      return false;  // live input passes through until the level starts
    case kPlaying: {
      rex::input::X_INPUT_GAMEPAD pad{};
      if (!g_player.Next(&pad)) {
        BenchFinish();
        g_state = kDone;
        return false;
      }
      BenchOnPoll();
      state->gamepad = pad;
      state->packet_number = ++g_packet;
      return true;
    }
    default:
      break;
  }

  // Recording control. The dossier screen waits for input, making it the
  // natural sync barrier: record from just after the player's dismiss press,
  // and replays anchored at the macro's own dismiss press align regardless
  // of load-time variance. Once in_level && g_saw_menu_input holds, first
  // wait for a real (nonzero) pad state -- the dossier-dismiss press itself
  // -- then arm the recorder at the very next all-neutral poll, so the
  // recording's first frame is the neutral gap right after the press rather
  // than the press (or its trailing hold) itself.
  if (g_record_armed) {
    if (!g_recording.load(std::memory_order_relaxed)) {
      if (in_level && g_saw_menu_input) {
        if (!g_saw_level_input) {
          if (IsNonzeroGamepadState(state->gamepad)) g_saw_level_input = true;
        } else if (!IsNonzeroGamepadState(state->gamepad) &&
                   g_recorder.Open(g_record_path)) {
          g_recording.store(true, std::memory_order_relaxed);
          REXKRNL_INFO("GEREPLAY recording started -> {}", g_record_path);
        }
      }
    } else if (!in_level) {
      g_recorder.Close();
      g_recording.store(false, std::memory_order_relaxed);
      g_record_armed = false;  // one segment per run
    } else {
      g_recorder.OnPoll(*state);
    }
  }

  return false;
}

}  // namespace

void ReplayInit(std::filesystem::path user_data_root, std::function<void()> quit_requester) {
  g_quit_requester = std::move(quit_requester);

  if (REXCVAR_GET(ge_replay_selftest)) {
    RunSelfTest();
  }

  g_record_path = REXCVAR_GET(ge_replay_record);
  g_record_armed = !g_record_path.empty();

  // Resolve playback/macro activation: an explicit cvar wins; otherwise fall
  // back to well-known files dropped in the user-data root (e.g. adb push,
  // where there's no config file or CLI to set cvars for an on-device bench
  // run).
  std::string play_path = REXCVAR_GET(ge_replay_play);
  std::string macro_path = REXCVAR_GET(ge_replay_macro);
  if (play_path.empty()) {
    auto candidate = user_data_root / "ge_replay.bin";
    if (std::filesystem::exists(candidate)) {
      play_path = rex::path_to_utf8(candidate);
    }
  }
  if (macro_path.empty()) {
    auto candidate = user_data_root / "ge_replay.macro";
    if (std::filesystem::exists(candidate)) {
      macro_path = rex::path_to_utf8(candidate);
    }
  }

  if (g_record_armed && !play_path.empty()) {
    // Record and play are mutually exclusive -- leave g_state at kIdle so
    // ReplayOnGetState's switch falls through to the (armed) recorder branch
    // untouched.
    REXKRNL_WARN("GEREPLAY record and play both set; recording wins");
  } else if (!play_path.empty() && g_player.Open(play_path)) {
    g_state = (!macro_path.empty() && g_macro.Load(macro_path)) ? kMacro : kWaitLevel;
  }

  if (g_record_armed) {
    std::atexit([] { g_recorder.Close(); });
  }

  rex::input::SetGetStateOverride(&ReplayOnGetState);
}

}  // namespace ge
