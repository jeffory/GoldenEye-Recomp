// ge - guest game-state bridge implementation. See ge_gamestate.h for the
// public contract. This translation unit is the ONLY place that reads/writes the
// player inventory in guest memory; everyone else goes through the snapshot.
//
// ---------------------------------------------------------------------------
// HOW THE READ/WRITE WORK (mechanism)
// ---------------------------------------------------------------------------
// Guest memory is big-endian PPC. We resolve a pointer to the player/inventory
// struct from a fixed guest global, then read the equipped-weapon id, the
// carried-weapon set, and per-weapon ammo out of that struct using the same
// getcb + big-endian LD helpers the rest of the project uses (ge_hooks.cpp).
// The values are assembled into a WeaponSnapshot and published through a seqlock
// so any thread can read a torn-free copy lock-free.
//
// The write path records a UI-thread equip request in an atomic and, on the next
// game frame, writes the game's per-frame weapon-select field (ST32) -- the same
// field a weapon-cycle input sets -- so the game runs its normal switch logic
// (animation, ammo check). Writes are gated to safe moments.
//
// ---------------------------------------------------------------------------
// GUEST LAYOUT CONSTANTS  ***NEEDS ON-DEVICE CONFIRMATION***
// ---------------------------------------------------------------------------
// The absolute guest addresses / struct offsets below are the SINGLE point of
// truth for where the bridge reads and writes. They are seeded from public
// GoldenEye 007 reverse-engineering notes (the carried-weapon / ammo layout the
// XBLA build inherits from the N64 original), but the exact XBLA guest addresses
// must be confirmed at runtime before the read/write paths return correct data.
//
// Until kPlayerPtrAddr is set to a real, confirmed value the bridge is INERT:
// OnFrame() publishes valid=false and drops equip requests, so the game is
// completely unaffected. Use the discovery diagnostic (cvar `ge_gamestate_diag`,
// below) to lock the constants in on-device -- this mirrors how the freeze
// watchdog / ge_dbg_now were used to reverse-engineer the GPU pipeline.
//
// Confirmation loop (on real hardware / a desktop build with the game files):
//   1. Enable `ge_gamestate_diag`. Set kProbeAddr to a candidate region.
//   2. In-game, cycle weapons / pick up ammo and watch the change log: the bytes
//      that move in lockstep with the equipped weapon id and ammo are the
//      offsets. Anchor the player-struct pointer the same way.
//   3. Fill the constants below, rebuild, verify the snapshot tracks the real
//      weapon and RequestEquipWeapon() switches it (the acceptance demo).

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "ge_gamestate.h"

#include "ge_init.h"   // PPCContext + generated guest function decls
#include <rex/cvar.h>  // REXCVAR_DEFINE_BOOL / REXCVAR_GET
#include <rex/hook.h>  // REXKRNL_INFO

// Discovery diagnostic toggle. Default OFF: in a shipping build the bridge does
// its cheap per-frame read with zero logging. Flip on to lock in the guest
// layout constants (see the file header's confirmation loop). Defined before
// first use (run_probe) so REXCVAR_GET resolves it -- mirrors ge_hooks.cpp.
REXCVAR_DEFINE_BOOL(ge_gamestate_diag, false, "Debug",
                    "GoldenEye: log the game-state bridge memory probe window "
                    "when it changes (used to reverse-engineer the inventory "
                    "layout on-device; OFF in shipping builds)");

namespace ge::gamestate {
namespace {

// --- big-endian guest-memory helpers (mirrors ge_hooks.cpp) -----------------
// LD8 / ST16 are provided for the on-device offset-confirmation work (byte-wide
// state flags, 16-bit ammo writes); not used by the inert default read path.
[[maybe_unused]] inline uint8_t LD8(uint8_t* b, uint32_t ga) { return b[ga]; }
inline uint16_t LD16(uint8_t* b, uint32_t ga) {
  uint16_t v; std::memcpy(&v, b + ga, 2); return __builtin_bswap16(v);
}
inline uint32_t LD32(uint8_t* b, uint32_t ga) {
  uint32_t v; std::memcpy(&v, b + ga, 4); return __builtin_bswap32(v);
}
[[maybe_unused]] inline void ST16(uint8_t* b, uint32_t ga, uint16_t val) {
  uint16_t v = __builtin_bswap16(val); std::memcpy(b + ga, &v, 2);
}
inline void ST32(uint8_t* b, uint32_t ga, uint32_t val) {
  uint32_t v = __builtin_bswap32(val); std::memcpy(b + ga, &v, 4);
}

// A guest address is plausible if it falls inside the title's mapped range. The
// XEX image + heap live in 0x82000000..0x90000000 in this title; anything else
// (0, tiny, or > membase span) is a stale/garbage pointer we must not chase.
inline bool plausible_guest_ptr(uint32_t ga) {
  return ga >= 0x82000000u && ga < 0x90000000u;
}

// ===========================================================================
// GUEST LAYOUT CONSTANTS  ***NEEDS ON-DEVICE CONFIRMATION*** (see file header)
// ===========================================================================

// Absolute guest address of the pointer to the current player/inventory struct.
// 0 => layout unconfirmed => bridge stays inert (valid=false), game unaffected.
inline constexpr uint32_t kPlayerPtrAddr = 0u;

// Offsets WITHIN the player/inventory struct kPlayerPtrAddr points at:
inline constexpr uint32_t kEquippedIdOff   = 0u;  // 32-bit current weapon id
inline constexpr uint32_t kHeldMaskOff     = 0u;  // 32-bit carried-weapon bitmask (0 => derive from ammo)
inline constexpr uint32_t kAmmoBaseOff     = 0u;  // start of per-slot ammo array
inline constexpr uint32_t kAmmoStride      = 2u;  // bytes between ammo entries (uint16 each)
inline constexpr uint32_t kNumSlots        = 0u;  // weapon slots to scan (<= kMaxWeaponSlots)

// Equip write target: the per-frame field the game reads to switch weapon (same
// one a weapon-cycle input sets). 0 => write path disabled (requests dropped).
inline constexpr uint32_t kEquipRequestOff = 0u;  // 32-bit "switch to this weapon" field

// Optional safety gate: a player-state field + an "alive & in normal play" mask.
// Equip writes only happen when (LD32(player + kStateOff) & kStateInPlayMask)
// matches. 0/0 => no extra gate beyond "a valid player struct exists".
inline constexpr uint32_t kStateOff        = 0u;
inline constexpr uint32_t kStateInPlayMask = 0u;

// ===========================================================================
// Discovery diagnostic. OFF in shipping builds. When on, logs a hex window of
// guest memory whenever it changes, so the layout constants above can be locked
// in on-device by watching what moves as you cycle weapons / pick up ammo.
// ===========================================================================
inline constexpr uint32_t kProbeAddr = 0u;   // candidate region to watch (0 => off)
inline constexpr uint32_t kProbeLen  = 64u;  // bytes to log (<= 256)

// ===========================================================================
// Thread-safe snapshot publication (single-writer seqlock).
//   Writer (game thread): seq -> odd, store fields, seq -> even.
//   Reader (any thread):  read seq (must be even), copy, re-read seq; retry if
//   it changed. Wait-free in practice (writer holds it for a few stores).
// ===========================================================================
std::atomic<uint32_t> g_seq{0};
WeaponSnapshot g_published{};  // guarded by g_seq

// Pending equip request from the UI thread. Packs a "has request" flag with the
// id so a single atomic exchange both reads and clears it on the game thread.
inline constexpr uint32_t kReqFlag = 0x80000000u;
std::atomic<uint32_t> g_pending_equip{0};

// Bridge frame counter (monotonic across pumped frames).
std::atomic<uint32_t> g_frame{0};

void publish(const WeaponSnapshot& s) {
  uint32_t seq = g_seq.load(std::memory_order_relaxed);
  g_seq.store(seq + 1, std::memory_order_release);          // -> odd: write in progress
  std::atomic_thread_fence(std::memory_order_release);
  g_published = s;
  std::atomic_thread_fence(std::memory_order_release);
  g_seq.store(seq + 2, std::memory_order_release);          // -> even: write complete
}

// --- discovery diagnostic: log the probe window when its contents change ----
void run_probe(uint8_t* base, uint32_t frame) {
  if (!REXCVAR_GET(ge_gamestate_diag) || kProbeAddr == 0u) return;
  static uint8_t last[256];
  static bool primed = false;
  uint32_t len = kProbeLen > 256u ? 256u : kProbeLen;
  if (!plausible_guest_ptr(kProbeAddr)) return;
  uint8_t* p = base + kProbeAddr;
  if (primed && std::memcmp(last, p, len) == 0) return;     // unchanged: stay quiet
  std::memcpy(last, p, len);
  primed = true;
  char hex[256 * 3 + 1];
  int o = 0;
  for (uint32_t i = 0; i < len && o + 3 < (int)sizeof(hex); i++)
    o += std::snprintf(hex + o, sizeof(hex) - o, "%02x ", p[i]);
  REXKRNL_INFO("GEGAMESTATE probe @{:#x} (frame {}): {}", kProbeAddr, frame, hex);
}

// --- read path: build a snapshot from guest memory --------------------------
WeaponSnapshot read_snapshot(uint8_t* base, uint32_t frame) {
  WeaponSnapshot s{};
  s.frame = frame;

  // Layout unconfirmed -> stay inert (and don't read address 0).
  if (kPlayerPtrAddr == 0u || !plausible_guest_ptr(kPlayerPtrAddr)) return s;

  uint32_t player = LD32(base, kPlayerPtrAddr);
  if (!plausible_guest_ptr(player)) return s;  // not in a level yet / stale ptr

  const uint32_t slots = kNumSlots > (uint32_t)kMaxWeaponSlots
                             ? (uint32_t)kMaxWeaponSlots : kNumSlots;

  s.equipped_id = static_cast<int32_t>(LD32(base, player + kEquippedIdOff));

  // Carried set: prefer the game's own bitmask; otherwise derive "held" from a
  // nonzero ammo entry (documented fallback for builds where the mask offset is
  // not yet identified).
  uint32_t mask = 0;
  for (uint32_t i = 0; i < slots; i++) {
    uint16_t a = LD16(base, player + kAmmoBaseOff + i * kAmmoStride);
    s.ammo[i] = a;
    bool held;
    if (kHeldMaskOff != 0u) {
      held = (LD32(base, player + kHeldMaskOff) & (1u << i)) != 0u;
    } else {
      held = a != 0u;
    }
    if (held) {
      mask |= (1u << i);
      if (s.held_count < kMaxWeaponSlots) s.held_ids[s.held_count++] = (uint8_t)i;
    }
  }
  s.held_mask = mask;

  // A real in-game player will have at least the equipped slot make sense. Treat
  // an out-of-range equipped id as "not in play" so we never publish garbage.
  s.valid = (s.equipped_id == kNoWeapon) ||
            (s.equipped_id >= 0 && s.equipped_id < (int32_t)kMaxWeaponSlots);
  return s;
}

// --- write path: apply a pending equip request at a safe moment -------------
void apply_equip(void* ppc_ctx, uint8_t* base, const WeaponSnapshot& s) {
  (void)ppc_ctx;  // reserved: a guest weapon-select call could use this ctx
  uint32_t req = g_pending_equip.exchange(0, std::memory_order_acq_rel);
  if (!(req & kReqFlag)) return;                 // nothing pending
  int32_t id = static_cast<int32_t>(req & ~kReqFlag);

  // Write path not yet wired (offset unconfirmed) or unsafe context -> drop the
  // request silently (it was already cleared above; a stale equip must not fire
  // a frame later in a menu/cutscene).
  if (kEquipRequestOff == 0u || kPlayerPtrAddr == 0u) return;
  if (!s.valid) return;                          // no live player
  if (id < 0 || id >= (int32_t)kMaxWeaponSlots) return;
  if (!(s.held_mask & (1u << id))) return;       // can't switch to a weapon not held

  uint32_t player = LD32(base, kPlayerPtrAddr);
  if (!plausible_guest_ptr(player)) return;

  // Safety gate: only switch during normal play (alive, not in a menu/cutscene).
  if (kStateOff != 0u && kStateInPlayMask != 0u) {
    if ((LD32(base, player + kStateOff) & kStateInPlayMask) != kStateInPlayMask) return;
  }

  // Write the game's per-frame weapon-select field; the game's own switch logic
  // (draw animation, ammo check) runs next frame, matching a weapon-cycle input.
  ST32(base, player + kEquipRequestOff, static_cast<uint32_t>(id));
}

}  // namespace

// ============================== public API =================================

WeaponSnapshot GetWeaponSnapshot() {
  for (;;) {
    uint32_t s1 = g_seq.load(std::memory_order_acquire);
    if (s1 & 1u) continue;                        // writer mid-update: spin
    std::atomic_thread_fence(std::memory_order_acquire);
    WeaponSnapshot copy = g_published;
    std::atomic_thread_fence(std::memory_order_acquire);
    uint32_t s2 = g_seq.load(std::memory_order_acquire);
    if (s1 == s2) return copy;                     // stable read
  }
}

void RequestEquipWeapon(int32_t weapon_id) {
  if (weapon_id < 0 || weapon_id >= kMaxWeaponSlots) return;
  g_pending_equip.store(kReqFlag | (uint32_t)weapon_id, std::memory_order_release);
}

void OnFrame(void* ppc_ctx, uint8_t* guest_base) {
  if (!guest_base) return;
  uint32_t frame = g_frame.fetch_add(1, std::memory_order_relaxed) + 1;

  run_probe(guest_base, frame);

  WeaponSnapshot s = read_snapshot(guest_base, frame);
  publish(s);
  apply_equip(ppc_ctx, guest_base, s);
}

}  // namespace ge::gamestate
