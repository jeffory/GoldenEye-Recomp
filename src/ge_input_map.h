// ge - gamepad button remapping (issue #15).
//
// This file is yours to edit. 'rexglue migrate' will NOT overwrite it.
//
// WHAT IT DOES
//   Rewrites the physical controller's button/trigger layout before the guest
//   ever sees it, so a player can (for example) fire with the right bumper
//   instead of the right trigger. Sticks are passed through untouched.
//
// DIRECTION OF THE MAPPING
//   Each cvar names a *guest* control and holds the *physical* control that
//   drives it -- the same "destination <- source" sense the keyboard binds
//   already use (ge_key_a = "Space" means "the guest's A comes from Space"):
//
//       ge_pad_rt = "RB"   ->  the guest's right trigger fires on physical RB
//
//   Every guest control has exactly one source. The table itself allows one
//   source to drive several destinations (a hand-edited config can say so, and
//   Apply handles it), but the rebinding UI never produces that: it goes
//   through Assign(), which SWAPS. Binding RB to the guest's right trigger
//   hands the guest's right bumper whatever the trigger used to have, so each
//   physical button still drives exactly one control. Assigning without the
//   swap leaves the source doing its old job as well as its new one -- one RB
//   press firing the gun *and* cycling weapons.
//
//   Defaults are identity, so an untouched install is bit-for-bit unchanged
//   (Apply short-circuits on the identity table).
//
// WHERE IT HOOKS, AND WHY IT HAS TO BE THERE
//   ApplyLive() runs from ReplayOnGetState, i.e. inside the SDK's
//   rex::input::SetGetStateOverride hook. That is the one point where the
//   merged state of every real input driver (Android NDK pad, SDL, MnK) is
//   visible, and -- critically -- it is *upstream* of the guest copying the
//   pad into its slot-0 buffer at GE_PAD0.
//
//   Everything the game synthesizes later writes straight into GE_PAD0 from
//   ge_inject_keyboard: the on-screen touch overlay, the ge_key_* keyboard
//   binds, and the weapon-switch walker's synthetic BTN_Y. Those are already
//   expressed in guest-button space and must NOT be remapped -- remapping a
//   player's Y would otherwise silently break weapon switching. Hooking
//   upstream of GE_PAD0 keeps that ordering correct by construction; hooking
//   at GE_PAD0 would not.
//
//   Running before the replay state machine also gives the right recording
//   semantics: recordings capture post-remap (effective) input, and playback
//   overwrites the pad afterwards, so a replay reproduces identically no
//   matter what bindings the machine playing it happens to have.
//
// RECOVERY
//   A mapping that makes the game unnavigable is undone with ge_pad_remap=false
//   (via ge.toml, --no-ge_pad_remap, or a pushed ge_cvars.txt on Android) --
//   one switch, rather than picking 16 cvars back apart.

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string_view>

#include <rex/input/input.h>

namespace ge::inputmap {

// The guest controls that can be re-sourced. Order is the Table index order;
// the button destinations come first so Apply can split on kLT.
enum class Dest : uint8_t {
  kA, kB, kX, kY,
  kLB, kRB, kL3, kR3,
  kStart, kBack,
  kDUp, kDDown, kDLeft, kDRight,
  kLT, kRT,          // analog destinations -- must stay last
  kCount
};
inline constexpr size_t kDestCount = static_cast<size_t>(Dest::kCount);
static_assert(kDestCount == 16, "Table is sized (and kept atomic-friendly) at 16 bytes");

// Physical controls a destination can be driven from. kNone = deliberately
// unbound. Values are persisted only via their string names, so the numbering
// is free to change.
enum class Source : uint8_t {
  kNone = 0,
  kA, kB, kX, kY,
  kLB, kRB, kL3, kR3,
  kStart, kBack,
  kDUp, kDDown, kDLeft, kDRight,
  kLT, kRT,
};

// A compiled mapping: for each Dest, the Source that drives it. Exactly 16
// bytes and trivially copyable so it can live in a std::atomic (same idiom as
// ge::PadState in ge_touchpad.h).
struct Table {
  uint8_t src[kDestCount];
};
static_assert(sizeof(Table) == 16, "keep Table atomic-width");

// Identity: every destination driven by its own physical control.
Table IdentityTable();

// Token <-> Source. Names are the ones users type into ge.toml and that the
// rebinding UI will display: "A".."Y", "LB", "RB", "LT", "RT", "L3", "R3",
// "Start", "Back", "DUp", "DDown", "DLeft", "DRight", and "" for unbound.
// Parsing is case-insensitive. Returns false for anything unrecognized.
bool ParseSource(std::string_view name, Source& out);
const char* SourceName(Source src);

// The cvar name carrying this destination's source (e.g. Dest::kRT ->
// "ge_pad_rt"), and the label the UI should show for it ("Right Trigger").
const char* DestCvar(Dest dest);
const char* DestLabel(Dest dest);

// Pure, side-effect-free core: build the guest-facing pad from the physical
// pad through `table`. Sticks and the guide button pass through unchanged.
// Safe to call with the identity table (returns `in` unchanged).
rex::input::X_INPUT_GAMEPAD Apply(const rex::input::X_INPUT_GAMEPAD& in, const Table& table);

// The live cvar-backed mapping. Cheap and callable from any thread; rebuilt
// only after one of the ge_pad_* cvars actually changes.
Table Current();

// Point `dest` at `source`, swapping with whichever destination `source` was
// driving so no physical button ends up doing two jobs (see the note above).
// This is what a rebind means to a player; writing the destination's cvar on
// its own is not. Returns false if nothing changed. Call from the UI thread.
bool Assign(Dest dest, Source source);

// Apply the live mapping in place. No-op when ge_pad_remap is off or the
// mapping is identity -- which is the shipping default.
//
// Also publishes the pre-remap snapshot and runs the menu-chord edge detector,
// so this must keep being called every poll even while the pause menu owns the
// controller. It is, because opening the menu does not pause the guest.
//
// user_index matters: the guest polls all four controller slots every frame,
// and the three empty ones arrive here as zeroed states. The remap and the
// menu suppression apply to every slot, but the physical snapshot and the
// chord's edge detection are slot-0 only -- letting the empty slots through
// interleaved a zero pad between every real one, which reset the chord's
// consecutive-poll counter and made it impossible to ever trigger.
void ApplyLive(uint32_t user_index, rex::input::X_INPUT_GAMEPAD& pad);

// --- pause-menu support -----------------------------------------------------
// The pause menu is the only UI a handheld can reach, and it needs three
// things the input path is uniquely placed to give it: the *physical* pad (so
// a rebind captures the button actually pressed, not what it is currently
// mapped to), a way to stop that input also reaching the game underneath, and
// a chord to open itself with on a device that has no keyboard.

// Just the digital state -- the rebindable controls, nothing else. Deliberately
// not an X_INPUT_GAMEPAD: this is published through a single 32-bit atomic, so
// readers never need a lock and no platform needs libatomic.
struct Snapshot {
  uint16_t buttons = 0;
  uint8_t left_trigger = 0;
  uint8_t right_trigger = 0;
};

// The most recent pad seen *before* remapping. Keeps updating while the guest
// is suppressed. Safe from any thread.
Snapshot LastPhysical();

// The first actuated source in `snapshot`, or kNone. Callers wanting a
// deliberate press should edge-detect against the previous snapshot rather
// than trusting a single sample -- see the CONTROLS tab's capture.
Source HeldSource(const Snapshot& snapshot);

// While set, the guest is handed a neutral pad: the menu has the controller
// and the game must not also act on it. LastPhysical() is unaffected.
void SetGuestInputSuppressed(bool suppressed);

// Invoked once per press when the ge_pad_menu_chord buttons go down together.
// Runs on the guest input thread -- defer anything real to the UI thread.
void SetMenuChordCallback(std::function<void()> callback);

// Register the change callbacks and log the active mapping. Call once, before
// the guest starts polling.
void Init();

}  // namespace ge::inputmap
