// ge - gamepad button remapping. See ge_input_map.h for the design and for
// why the hook sits where it does.
//
// This file is yours to edit. 'rexglue migrate' will NOT overwrite it.

#include "ge_input_map.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <string>

#include <rex/cvar.h>
#include <rex/logging.h>

// Master switch. The mapping below defaults to identity, so this only matters
// once a player has changed something -- it exists so a controller layout that
// makes the menus unnavigable can be undone with one cvar instead of 16 (see
// "RECOVERY" in the header).
REXCVAR_DEFINE_BOOL(ge_pad_remap, true, "Input/Gamepad",
                    "Apply the gamepad button remap below");

// The button combination that opens the pause menu. Android has no keyboard,
// so without this the menu -- and therefore every setting, including the
// remap below -- is unreachable on a handheld. Two sources joined by '+';
// empty disables it. Select (Back) is the default: it is a single, easy press
// on a handheld, and the stick-click pair that was here first turned out not
// to register on an Ayn Thor. The button is withheld from the game while the
// chord is down, so it opens our menu rather than doing both.
REXCVAR_DEFINE_STRING(ge_pad_menu_chord, "Back", "Input/Gamepad",
                      "Controller button (or A+B combination) that opens the pause menu");

// One cvar per guest control, holding the physical control that drives it.
// Category is deliberately NOT under "Input/Keybinds/", whose entries the SDK
// settings overlay renders with a *keyboard* capture widget -- that would
// happily write "F5" into a pad source.
REXCVAR_DEFINE_STRING(ge_pad_a, "A", "Input/Gamepad", "A button");
REXCVAR_DEFINE_STRING(ge_pad_b, "B", "Input/Gamepad", "B button");
REXCVAR_DEFINE_STRING(ge_pad_x, "X", "Input/Gamepad", "X button");
REXCVAR_DEFINE_STRING(ge_pad_y, "Y", "Input/Gamepad", "Y button");
REXCVAR_DEFINE_STRING(ge_pad_lb, "LB", "Input/Gamepad", "Left bumper");
REXCVAR_DEFINE_STRING(ge_pad_rb, "RB", "Input/Gamepad", "Right bumper");
REXCVAR_DEFINE_STRING(ge_pad_l3, "L3", "Input/Gamepad", "Left stick press");
REXCVAR_DEFINE_STRING(ge_pad_r3, "R3", "Input/Gamepad", "Right stick press");
REXCVAR_DEFINE_STRING(ge_pad_start, "Start", "Input/Gamepad", "Start button");
REXCVAR_DEFINE_STRING(ge_pad_back, "Back", "Input/Gamepad", "Back button");
REXCVAR_DEFINE_STRING(ge_pad_dup, "DUp", "Input/Gamepad", "D-pad up");
REXCVAR_DEFINE_STRING(ge_pad_ddown, "DDown", "Input/Gamepad", "D-pad down");
REXCVAR_DEFINE_STRING(ge_pad_dleft, "DLeft", "Input/Gamepad", "D-pad left");
REXCVAR_DEFINE_STRING(ge_pad_dright, "DRight", "Input/Gamepad", "D-pad right");
REXCVAR_DEFINE_STRING(ge_pad_lt, "LT", "Input/Gamepad", "Left trigger");
REXCVAR_DEFINE_STRING(ge_pad_rt, "RT", "Input/Gamepad", "Right trigger");

namespace ge::inputmap {

namespace {

using rex::input::X_INPUT_GAMEPAD;

// XInput's XINPUT_GAMEPAD_TRIGGER_THRESHOLD. Used when an analog trigger
// drives a digital destination.
constexpr uint8_t kTriggerThreshold = 30;

struct DestInfo {
  Dest dest;
  Source identity;   // the source this destination has by default
  const char* cvar;
  const char* label;
};

// Declaration order must match the Dest enum -- checked at Init().
constexpr DestInfo kDests[] = {
    {Dest::kA,      Source::kA,      "ge_pad_a",      "A"},
    {Dest::kB,      Source::kB,      "ge_pad_b",      "B"},
    {Dest::kX,      Source::kX,      "ge_pad_x",      "X"},
    {Dest::kY,      Source::kY,      "ge_pad_y",      "Y"},
    {Dest::kLB,     Source::kLB,     "ge_pad_lb",     "Left Bumper"},
    {Dest::kRB,     Source::kRB,     "ge_pad_rb",     "Right Bumper"},
    {Dest::kL3,     Source::kL3,     "ge_pad_l3",     "Left Stick (L3)"},
    {Dest::kR3,     Source::kR3,     "ge_pad_r3",     "Right Stick (R3)"},
    {Dest::kStart,  Source::kStart,  "ge_pad_start",  "Start"},
    {Dest::kBack,   Source::kBack,   "ge_pad_back",   "Back"},
    {Dest::kDUp,    Source::kDUp,    "ge_pad_dup",    "D-Pad Up"},
    {Dest::kDDown,  Source::kDDown,  "ge_pad_ddown",  "D-Pad Down"},
    {Dest::kDLeft,  Source::kDLeft,  "ge_pad_dleft",  "D-Pad Left"},
    {Dest::kDRight, Source::kDRight, "ge_pad_dright", "D-Pad Right"},
    {Dest::kLT,     Source::kLT,     "ge_pad_lt",     "Left Trigger"},
    {Dest::kRT,     Source::kRT,     "ge_pad_rt",     "Right Trigger"},
};
static_assert(sizeof(kDests) / sizeof(kDests[0]) == kDestCount, "kDests must cover every Dest");

// kDests is indexed by Dest throughout, so a reordering would silently
// mis-map controls. Catch it at build time.
constexpr bool DestsAreInDeclOrder() {
  for (size_t i = 0; i < kDestCount; ++i) {
    if (static_cast<size_t>(kDests[i].dest) != i) return false;
  }
  return true;
}
static_assert(DestsAreInDeclOrder(), "kDests must be indexed by Dest");

struct SourceInfo {
  Source source;
  const char* name;
  uint16_t button;  // X_INPUT_GAMEPAD_* bit, or 0 for the analog triggers
};

constexpr SourceInfo kSources[] = {
    {Source::kNone,   "",       0},
    {Source::kA,      "A",      rex::input::X_INPUT_GAMEPAD_A},
    {Source::kB,      "B",      rex::input::X_INPUT_GAMEPAD_B},
    {Source::kX,      "X",      rex::input::X_INPUT_GAMEPAD_X},
    {Source::kY,      "Y",      rex::input::X_INPUT_GAMEPAD_Y},
    {Source::kLB,     "LB",     rex::input::X_INPUT_GAMEPAD_LEFT_SHOULDER},
    {Source::kRB,     "RB",     rex::input::X_INPUT_GAMEPAD_RIGHT_SHOULDER},
    {Source::kL3,     "L3",     rex::input::X_INPUT_GAMEPAD_LEFT_THUMB},
    {Source::kR3,     "R3",     rex::input::X_INPUT_GAMEPAD_RIGHT_THUMB},
    {Source::kStart,  "Start",  rex::input::X_INPUT_GAMEPAD_START},
    {Source::kBack,   "Back",   rex::input::X_INPUT_GAMEPAD_BACK},
    {Source::kDUp,    "DUp",    rex::input::X_INPUT_GAMEPAD_DPAD_UP},
    {Source::kDDown,  "DDown",  rex::input::X_INPUT_GAMEPAD_DPAD_DOWN},
    {Source::kDLeft,  "DLeft",  rex::input::X_INPUT_GAMEPAD_DPAD_LEFT},
    {Source::kDRight, "DRight", rex::input::X_INPUT_GAMEPAD_DPAD_RIGHT},
    {Source::kLT,     "LT",     0},
    {Source::kRT,     "RT",     0},
};

const SourceInfo& Info(Source src) {
  for (const auto& info : kSources) {
    if (info.source == src) return info;
  }
  return kSources[0];  // kNone
}

// The X_INPUT bit a button destination occupies in the guest-facing pad. By
// definition that is the bit of its own physical control, which is exactly
// what its identity source names -- the two cannot diverge for a button
// destination, and it is 0 for the two analog ones (which never use this).
uint16_t DestButton(const DestInfo& info) { return Info(info.identity).button; }

// Is this source actuated on the incoming pad? Analog triggers cross
// kTriggerThreshold; everything else is a button bit.
bool SourceHeld(const X_INPUT_GAMEPAD& in, Source src) {
  if (src == Source::kNone) return false;
  if (src == Source::kLT) return in.left_trigger >= kTriggerThreshold;
  if (src == Source::kRT) return in.right_trigger >= kTriggerThreshold;
  return (static_cast<uint16_t>(in.buttons) & Info(src).button) != 0;
}

// The analog level a source contributes to a trigger destination. A digital
// source pushes the trigger to full scale, which is what the guest's own
// "trigger pressed" checks expect.
uint8_t SourceLevel(const X_INPUT_GAMEPAD& in, Source src) {
  if (src == Source::kLT) return in.left_trigger;
  if (src == Source::kRT) return in.right_trigger;
  return SourceHeld(in, src) ? 0xFF : 0;
}

// --- live table -------------------------------------------------------------
// The compiled table is cached per thread and rebuilt whenever the generation
// counter moves. Change callbacks (UI thread, fired from inside the cvar
// registry lock -- so they must do nothing but this bump) move the counter;
// each reader notices on its next call and recompiles its own private copy.
//
// No shared mutable table means no lock and no torn read on the guest input
// path. An earlier version kept a std::atomic<Table>, but a 16-byte atomic
// needs __atomic_load_16/__atomic_store_16 out of libatomic, which is a
// portability liability across the Linux/Windows/NDK toolchains for a value
// that changes only when someone edits a binding.
std::atomic<uint32_t> g_generation{1};
bool g_initialized = false;

Table BuildFromCvars() {
  Table table = IdentityTable();
  for (const auto& info : kDests) {
    const std::string value = rex::cvar::GetFlagByName(info.cvar);
    Source parsed = Source::kNone;
    if (value.empty()) {
      // Deliberately unbound.
      table.src[static_cast<size_t>(info.dest)] = static_cast<uint8_t>(Source::kNone);
    } else if (ParseSource(value, parsed)) {
      table.src[static_cast<size_t>(info.dest)] = static_cast<uint8_t>(parsed);
    } else {
      // Junk in the config: keep the stock binding rather than silently
      // dropping a control, and say so.
      REXKRNL_WARN("GEPADMAP {} = '{}' is not a controller button; using {}", info.cvar, value,
                   SourceName(info.identity));
    }
  }
  return table;
}

// --- pause-menu support -----------------------------------------------------
// The physical pad is republished every poll into one 32-bit atomic: buttons
// in bits 0..15, left trigger in 16..23, right trigger in 24..31. A single
// lock-free word keeps the UI thread's reads trivially consistent without
// dragging in a wider atomic (see the note on Table above).
std::atomic<uint32_t> g_physical{0};
std::atomic<bool> g_suppressed{false};
std::function<void()> g_chord_callback;  // set once at init, read on the input thread

// Chord debounce state (input thread only, ~60 polls/s).
//
// A physical stick-click or Select button bounces, and the poll rate is fast
// enough to see individual bounces as separate presses. Without this, one
// press could toggle the menu open and straight back shut -- which looks
// exactly like the chord not working at all.
bool g_chord_was_down = false;
int g_chord_held_polls = 0;    // consecutive polls with the combination down
int g_chord_cooldown = 0;      // polls remaining before another fire is allowed
constexpr int kChordHoldPolls = 3;   // ~50ms: rides out contact bounce
constexpr int kChordCooldown = 30;   // ~500ms between fires

uint32_t PackSnapshot(const X_INPUT_GAMEPAD& pad) {
  return static_cast<uint32_t>(static_cast<uint16_t>(pad.buttons)) |
         (static_cast<uint32_t>(pad.left_trigger) << 16) |
         (static_cast<uint32_t>(pad.right_trigger) << 24);
}

// Rising edge of "every chord button held at once". Re-armed only after the
// combination is fully released, so holding it does not retrigger.
//
// Also clears the chord's own buttons from `pad` while the full combination is
// held, so opening the menu does not simultaneously press those buttons in the
// game. A chord button pressed on its own is untouched and still plays.
void CheckMenuChord(X_INPUT_GAMEPAD& pad) {
  if (!g_chord_callback) return;
  const std::string spec = REXCVAR_GET(ge_pad_menu_chord);
  if (spec.empty()) {
    g_chord_was_down = false;
    return;
  }

  bool all_held = true;
  bool any_parsed = false;
  uint16_t chord_bits = 0;
  size_t start = 0;
  while (start <= spec.size()) {
    const size_t plus = spec.find('+', start);
    const size_t end = (plus == std::string::npos) ? spec.size() : plus;
    std::string token = spec.substr(start, end - start);
    // Tolerate spaces around the separator ("Back + Start").
    const size_t first = token.find_first_not_of(" \t");
    if (first != std::string::npos) {
      token = token.substr(first, token.find_last_not_of(" \t") - first + 1);
      Source source = Source::kNone;
      if (ParseSource(token, source) && source != Source::kNone) {
        any_parsed = true;
        if (!SourceHeld(pad, source)) all_held = false;
        chord_bits = static_cast<uint16_t>(chord_bits | Info(source).button);
      }
    }
    if (plus == std::string::npos) break;
    start = plus + 1;
  }

  const bool down = any_parsed && all_held;
  if (down) {
    pad.buttons = static_cast<uint16_t>(static_cast<uint16_t>(pad.buttons) & ~chord_bits);
  }
  if (g_chord_cooldown > 0) --g_chord_cooldown;

  // Debounced rising edge: the combination has to be down for kChordHoldPolls
  // in a row, having been fully released since the last fire, and the cooldown
  // has to have expired.
  g_chord_held_polls = down ? (g_chord_held_polls + 1) : 0;
  if (!down) g_chord_was_down = false;
  if (!g_chord_was_down && g_chord_held_polls == kChordHoldPolls && g_chord_cooldown == 0) {
    g_chord_was_down = true;
    g_chord_cooldown = kChordCooldown;
    REXKRNL_INFO("GEPADMAP menu chord '{}' fired", spec);
    g_chord_callback();
  }
}

}  // namespace

Table IdentityTable() {
  Table table{};
  for (const auto& info : kDests) {
    table.src[static_cast<size_t>(info.dest)] = static_cast<uint8_t>(info.identity);
  }
  return table;
}

bool ParseSource(std::string_view name, Source& out) {
  std::string lowered(name);
  std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  for (const auto& info : kSources) {
    std::string candidate(info.name);
    std::transform(candidate.begin(), candidate.end(), candidate.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (candidate == lowered) {
      out = info.source;
      return true;
    }
  }
  return false;
}

const char* SourceName(Source src) { return Info(src).name; }

const char* DestCvar(Dest dest) { return kDests[static_cast<size_t>(dest)].cvar; }
const char* DestLabel(Dest dest) { return kDests[static_cast<size_t>(dest)].label; }

X_INPUT_GAMEPAD Apply(const X_INPUT_GAMEPAD& in, const Table& table) {
  static const Table identity = IdentityTable();
  if (std::equal(std::begin(table.src), std::end(table.src), std::begin(identity.src))) {
    return in;  // shipping default -- bit-for-bit passthrough
  }

  // Built fresh from `in` rather than edited in place: with several
  // destinations legitimately sharing one source, an in-place OR would leave
  // bits stuck on from whichever destination was rewritten first.
  X_INPUT_GAMEPAD out{};
  out.thumb_lx = in.thumb_lx;
  out.thumb_ly = in.thumb_ly;
  out.thumb_rx = in.thumb_rx;
  out.thumb_ry = in.thumb_ry;

  // The guide button is a system button, not a game control -- it is not
  // remappable and passes straight through.
  uint16_t buttons = static_cast<uint16_t>(static_cast<uint16_t>(in.buttons) &
                                           rex::input::X_INPUT_GAMEPAD_GUIDE);

  for (const auto& info : kDests) {
    const auto src = static_cast<Source>(table.src[static_cast<size_t>(info.dest)]);
    if (info.dest == Dest::kLT) {
      out.left_trigger = SourceLevel(in, src);
    } else if (info.dest == Dest::kRT) {
      out.right_trigger = SourceLevel(in, src);
    } else if (SourceHeld(in, src)) {
      buttons = static_cast<uint16_t>(buttons | DestButton(info));
    }
  }

  out.buttons = buttons;
  return out;
}

bool Assign(Dest dest, Source source) {
  const Table table = BuildFromCvars();
  const auto previous = static_cast<Source>(table.src[static_cast<size_t>(dest)]);
  if (previous == source) {
    return false;
  }

  // Hand the destination(s) currently driven by `source` whatever `dest` had,
  // so the two controls trade places. Normally there is exactly one such
  // destination and this is a clean swap; a hand-edited config can name the
  // same source twice, in which case they all move together -- no worse than
  // the state they were already in.
  for (const auto& info : kDests) {
    if (info.dest == dest) continue;
    if (static_cast<Source>(table.src[static_cast<size_t>(info.dest)]) == source) {
      rex::cvar::SetFlagByName(info.cvar, SourceName(previous));
    }
  }
  rex::cvar::SetFlagByName(DestCvar(dest), SourceName(source));
  // Invalidate the cached table here rather than relying on the change
  // callbacks: those only exist once Init() has run, and a rebind must be
  // visible to the very next poll no matter what registered what.
  g_generation.fetch_add(1, std::memory_order_release);
  REXKRNL_INFO("GEPADMAP {} <- {} (swapped {} out)", DestLabel(dest), SourceName(source),
               SourceName(previous));
  return true;
}

Table Current() {
  thread_local Table cached = IdentityTable();
  thread_local uint32_t cached_generation = 0;
  const uint32_t generation = g_generation.load(std::memory_order_acquire);
  if (cached_generation != generation) {
    cached = BuildFromCvars();
    cached_generation = generation;
  }
  return cached;
}

Snapshot LastPhysical() {
  const uint32_t packed = g_physical.load(std::memory_order_acquire);
  Snapshot snapshot;
  snapshot.buttons = static_cast<uint16_t>(packed & 0xFFFF);
  snapshot.left_trigger = static_cast<uint8_t>((packed >> 16) & 0xFF);
  snapshot.right_trigger = static_cast<uint8_t>((packed >> 24) & 0xFF);
  return snapshot;
}

Source HeldSource(const Snapshot& snapshot) {
  for (const auto& info : kSources) {
    if (info.source == Source::kNone) continue;
    if (info.source == Source::kLT) {
      if (snapshot.left_trigger >= kTriggerThreshold) return info.source;
    } else if (info.source == Source::kRT) {
      if (snapshot.right_trigger >= kTriggerThreshold) return info.source;
    } else if ((snapshot.buttons & info.button) != 0) {
      return info.source;
    }
  }
  return Source::kNone;
}

void SetGuestInputSuppressed(bool suppressed) {
  g_suppressed.store(suppressed, std::memory_order_release);
}

void SetMenuChordCallback(std::function<void()> callback) {
  const bool armed = static_cast<bool>(callback);
  g_chord_callback = std::move(callback);
  // Worth a line: on a handheld this chord is the only route into the menu, so
  // "was it even armed?" is the first question when it appears not to work.
  REXKRNL_INFO("GEPADMAP menu chord {} (combination '{}')", armed ? "armed" : "disarmed",
               REXCVAR_GET(ge_pad_menu_chord));
}

void ApplyLive(uint32_t user_index, X_INPUT_GAMEPAD& pad) {
  // Slot 0 only. The other three slots are polled every frame too and, with no
  // device attached, arrive here zeroed -- treating them as real input would
  // (and did) interleave a released pad between every genuine one.
  if (user_index == 0) {
    // Publish first: the rebinding UI must see the button the player actually
    // pressed, before any remap rewrites it, and it must keep seeing it while
    // the guest is suppressed.
    g_physical.store(PackSnapshot(pad), std::memory_order_release);

    // Bounded input diagnostic. On a handheld the chord is the only route into
    // the settings, so when it "does nothing" the first thing to establish is
    // whether the button reaches the runtime at all -- a question that
    // otherwise needs a rebuild to answer. Logs the first few presses, then
    // goes quiet.
    {
      static uint16_t last_buttons = 0;   // input thread only
      static int logged = 0;
      const uint16_t buttons = static_cast<uint16_t>(pad.buttons);
      const uint16_t pressed = static_cast<uint16_t>(buttons & ~last_buttons);
      last_buttons = buttons;
      if (pressed && logged < 12) {
        ++logged;
        REXKRNL_INFO("GEPADMAP pad press 0x{:04X} ({})", pressed,
                     SourceName(HeldSource({pressed, 0, 0})));
      }
    }

    CheckMenuChord(pad);
  }

  if (g_suppressed.load(std::memory_order_acquire)) {
    // The menu owns the controller; the game gets a neutral pad so navigating
    // a menu does not also play the game behind it.
    pad = X_INPUT_GAMEPAD{};
    return;
  }

  if (!REXCVAR_GET(ge_pad_remap)) return;
  pad = Apply(pad, Current());
}

void Init() {
  if (g_initialized) return;
  g_initialized = true;

  // Callbacks only flip the dirty flag: they run while the cvar registry lock
  // is held, so they must not call back into the registry.
  for (const auto& info : kDests) {
    rex::cvar::RegisterChangeCallback(
        info.cvar, [](std::string_view, std::string_view) {
          g_generation.fetch_add(1, std::memory_order_release);
        });
  }

  const Table table = Current();
  const Table identity = IdentityTable();
  if (!std::equal(std::begin(table.src), std::end(table.src), std::begin(identity.src))) {
    for (const auto& info : kDests) {
      const auto src = static_cast<Source>(table.src[static_cast<size_t>(info.dest)]);
      if (src != info.identity) {
        REXKRNL_INFO("GEPADMAP {} <- {}", info.label, src == Source::kNone ? "(unbound)"
                                                                           : SourceName(src));
      }
    }
  }
}

}  // namespace ge::inputmap
