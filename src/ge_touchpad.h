// ge - on-screen touch controls -> synthesized Xbox 360 pad state.
//
// This file is yours to edit. 'rexglue migrate' will NOT overwrite it.
//
// PURPOSE
//   Ordinary Android phones/tablets have no controller. A translucent Java
//   overlay (GoldenEyeActivity / TouchControlsView) draws a virtual gamepad and,
//   each time the touch state changes, forwards a fully-formed X_INPUT_GAMEPAD
//   over JNI (see ge_android_touch.cpp) into the TouchPad singleton here.
//
//   The game-thread input hook (ge_inject_keyboard in ge_hooks.cpp) reads this
//   snapshot once per controller poll and ORs it into the guest slot-0 pad
//   buffer -- exactly the mechanism the keyboard/mouse path already uses, so
//   touch input combines cleanly with (the absence of) a real pad and needs no
//   new SDK input driver.
//
// THREAD-SAFETY CONTRACT
//   - SetState() : called from the Android UI thread (JNI). Lock-free publish.
//   - GetState() : called from the guest/game thread once per poll. Lock-free.
//   Both go through a single std::atomic<PadState>, so a reader never sees a
//   torn mix of two updates.
//
// This header pulls in nothing from the recompiler/PPC generated code, and the
// state type is platform-neutral, so it compiles everywhere. Only the JNI
// binding (ge_android_touch.cpp) is Android-only; on desktop TouchPad simply
// stays at its zeroed default and the injection is a no-op.

#pragma once

#include <atomic>
#include <cstdint>

namespace ge {

// A synthesized Xbox 360 gamepad frame. Field order/units match the guest
// X_INPUT_GAMEPAD (buttons bitfield, 0..255 triggers, +/-32767 thumbsticks);
// the injection hook byte-swaps into the big-endian guest buffer. POD +
// trivially copyable so it can live in a std::atomic. Padded to 16 bytes so the
// atomic is a natural, (typically) lock-free width.
struct PadState {
  uint16_t buttons = 0;        // X_INPUT_GAMEPAD_* bits (host endianness)
  uint8_t left_trigger = 0;    // 0..255
  uint8_t right_trigger = 0;   // 0..255
  int16_t thumb_lx = 0;        // -32767..32767
  int16_t thumb_ly = 0;
  int16_t thumb_rx = 0;
  int16_t thumb_ry = 0;
  uint32_t reserved = 0;       // pad to 16 bytes
};

class TouchPad {
 public:
  static TouchPad& Get();

  // Publish the latest synthesized pad frame (Android UI/JNI thread).
  void SetState(const PadState& state) {
    state_.store(state, std::memory_order_relaxed);
  }

  // Read the most recent pad frame (guest/game thread).
  PadState GetState() const { return state_.load(std::memory_order_relaxed); }

  // True if any control is currently actuated -- lets the injection hook skip
  // all guest writes (and thus never fight a real pad) when nothing is touched.
  bool Active() const {
    const PadState s = GetState();
    return s.buttons != 0 || s.left_trigger != 0 || s.right_trigger != 0 ||
           s.thumb_lx != 0 || s.thumb_ly != 0 || s.thumb_rx != 0 ||
           s.thumb_ry != 0;
  }

 private:
  TouchPad() = default;
  std::atomic<PadState> state_{PadState{}};
};

#if defined(__ANDROID__)
// Register the touch-controls JNI methods with ART, mirroring
// AndroidDsRegisterNatives(). Called once from GeApp::OnConfigurePaths, next to
// the dual-screen registration. See ge_android_touch.cpp.
void AndroidTouchRegisterNatives();
#endif

}  // namespace ge
