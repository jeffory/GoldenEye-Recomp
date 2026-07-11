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
// Recorder/replayer bodies land in later tasks; ReplayOnGetState always
// returns false here (never substitutes a state), so this skeleton has no
// gameplay effect with the harness cvars at their defaults.

#include "ge_replay.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>
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
extern "C" uint64_t rex_ge_guest_refresh_count();

#pragma pack(push, 1)
struct ReplayHeader {
  uint32_t magic;         // 'GERP' = 0x50524547 little-endian
  uint32_t version;       // 1
  uint32_t poll_hz_hint;  // informational (from the probe, e.g. 60)
  uint32_t reserved;      // 0
  uint64_t level_flag_addr;  // 0x8272B424
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
    ReplayHeader h{kReplayMagic, 1, 60, 0, 0x8272B424ull, 0};
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

namespace ge {
namespace {

std::function<void()> g_quit_requester;

// --- recording state -------------------------------------------------------
Recorder g_recorder;
std::atomic<bool> g_recording{false};
bool g_record_armed = false;
std::string g_record_path;
bool g_saw_menu_input = false;

// --- self-test -------------------------------------------------------------
void RunSelfTest() {
  const std::string path = "/tmp/claude-1000/-home-keith-Projects-GoldenEye-Recomp/e13e463a-cbdf-4b94-a278-02419ca85208/scratchpad/ge_replay_selftest.bin";
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
  REXKRNL_INFO("GEREPLAY PROBE polls/s={} submits/s={} polls_per_frame={:.2f} in_level={}",
               dp, ds, ds ? double(dp) / double(ds) : 0.0, GeInLevel() ? 1 : 0);
}

bool ReplayOnGetState(uint32_t user_index, rex::input::X_INPUT_STATE* state) {
  if (user_index != 0) {
    return false;
  }
  if (REXCVAR_GET(ge_replay_probe)) {
    ProbeOnPoll();
  }

  // Track menu input to gate recording (Attract mode sets the in-level flag too;
  // require real menu input first so an idle title screen can't start the recording.)
  if (!GeInLevel()) {
    if (state->gamepad.buttons || state->gamepad.left_trigger || state->gamepad.right_trigger ||
        state->gamepad.thumb_lx || state->gamepad.thumb_ly ||
        state->gamepad.thumb_rx || state->gamepad.thumb_ry) {
      g_saw_menu_input = true;
    }
  }

  // Recording control
  if (g_record_armed) {
    bool in_level = GeInLevel();
    if (!g_recording.load(std::memory_order_relaxed)) {
      if (in_level && g_saw_menu_input && g_recorder.Open(g_record_path)) {
        g_recording.store(true, std::memory_order_relaxed);
        REXKRNL_INFO("GEREPLAY recording started -> {}", g_record_path);
      }
    } else if (!in_level) {
      g_recorder.Close();
      g_recording.store(false, std::memory_order_relaxed);
      g_record_armed = false;  // one segment per run
    } else {
      g_recorder.OnPoll(*state);
    }
  }

  return false;  // recorder/replayer land here in later tasks
}

}  // namespace

void ReplayInit(std::function<void()> quit_requester) {
  g_quit_requester = std::move(quit_requester);

  if (REXCVAR_GET(ge_replay_selftest)) {
    RunSelfTest();
  }

  g_record_path = REXCVAR_GET(ge_replay_record);
  g_record_armed = !g_record_path.empty();
  if (g_record_armed) {
    std::atexit([] { g_recorder.Close(); });
  }

  rex::input::SetGetStateOverride(&ReplayOnGetState);
}

}  // namespace ge
