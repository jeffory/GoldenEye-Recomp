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
#include <cstdint>
#include <cstdio>
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

namespace ge {
namespace {

std::function<void()> g_quit_requester;

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
  return false;  // recorder/replayer land here in later tasks
}

}  // namespace

void ReplayInit(std::function<void()> quit_requester) {
  g_quit_requester = std::move(quit_requester);
  rex::input::SetGetStateOverride(&ReplayOnGetState);
}

}  // namespace ge
