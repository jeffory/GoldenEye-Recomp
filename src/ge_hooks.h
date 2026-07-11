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

// True while the guest is actually in a level (solo full-screen view; see the
// 0x8272B424 readers in ge_hooks.cpp), false at the menu/attract loop or
// before guest memory is mapped. Used by the input-replay probe
// (ge_replay.cpp) to tag poll-cadence samples with menu vs. in-level context.
// Safe to call from any thread.
bool GeInLevel();

// Diagnostic-only raw-value accessors backing GeInLevel()'s discriminator, so
// ge_replay.cpp's GEREPLAY PROBE line can log the underlying guest globals
// instead of just the derived bool. Same guarded-base pattern as GeInLevel();
// return 0 if guest memory isn't mapped yet. Safe to call from any thread.
uint32_t GeDbgLevelFlag();  // raw u32 @ 0x8272B424 (solo full-screen screen flag)
uint32_t GeDbgPlayerPtr();  // raw u32 @ 0x82F1FAAC (current-player table ptr)

}  // namespace ge
