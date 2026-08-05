// ge - small standalone header exposing a guest-state accessor implemented in
// ge_hooks.cpp to other translation units (ge_replay.cpp's poll-cadence
// probe). No ge_hooks.h existed before this; ge_hooks.cpp's other exports are
// forward-declared ad hoc wherever they're consumed (see ge_app.h's
// LaunchSelfDetached()/InitMouseLook() declarations) because the file's ~2200
// guest hooks live at global scope for the PPC recompiler's hook table. This
// header follows ge_gamestate.h's pattern instead (a tiny bridge header that
// pulls in nothing from the recompiler/PPC generated code) since GeInLevel()
// is a plain accessor, not a hook.
//
// This file is yours to edit. 'rexglue migrate' will NOT overwrite it.

#pragma once

#include <cstdint>

namespace ge {

// True while a mission is loading or running: the current-stage-number
// global at 0x82F1E704 is != 90 (see ge_hooks.cpp). False on every frontend
// screen INCLUDING the attract demo (stage reads 90 there too) or before
// guest memory is mapped. Used by the input-replay probe (ge_replay.cpp) to
// tag poll-cadence samples with menu vs. in-level context, and as the
// recorder/replayer state machine's sync barrier.
// Safe to call from any thread.
bool GeInLevel();

// Diagnostic-only raw-value accessors backing GeInLevel()'s discriminator, so
// ge_replay.cpp's GEREPLAY PROBE line can log the underlying guest globals
// instead of just the derived bool. Same guarded-base pattern as GeInLevel();
// return 0 if guest memory isn't mapped yet. Safe to call from any thread.
// task-6 RE-hunt finalists (evidence: .superpowers/sdd/task-6-re-hunt.md;
// full semantics at the definitions in ge_hooks.cpp):
uint32_t GeDbgLevelFlag();  // 0x82C8681C frontend-overlay alpha (0 = attract demo playing)
uint32_t GeDbgPlayerPtr();  // 0x8272B3A8 boot-frontend session flag (one-way 1->0)
uint32_t GeDbgStageNum();   // 0x82F1E704 current stage number (90 = frontend; GeInLevel()'s source)

}  // namespace ge
