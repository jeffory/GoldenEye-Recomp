// ge - input record/replay/bench harness (skeleton + poll-cadence probe).
//
// This file is yours to edit. 'rexglue migrate' will NOT overwrite it.

#pragma once

#include <functional>

namespace ge {

// Install the input record/replay/bench harness (reads the ge_replay_* cvars
// and the user-data activation files). quit_requester is invoked (once) when
// ge_bench_exit is set and the replay finishes; it must be safe to call from
// a guest thread (defer to the UI thread internally).
void ReplayInit(std::function<void()> quit_requester);

}  // namespace ge
