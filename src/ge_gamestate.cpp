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
// The equip path records a UI-thread request in an atomic (RequestEquipWeapon);
// the actuator lives in ge_hooks.cpp, which peeks the pending target
// (PeekEquipRequest) and pulses the game's native Y weapon-switch input until the
// equipped weapon reaches it, then clears it (ClearEquipRequest). A direct
// guest-memory write was tried and abandoned -- writing the equipped-id block
// corrupts it and the game does not poll a "desired weapon" field (see
// docs/HANDOFF-weapon-switch-direct-call.md). This TU no longer writes guest
// weapon state; it only reads/publishes the snapshot.
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

// Fault-safe guest reads for the live per-frame path (Linux + Android). Reading
// a stray guest pointer via a raw deref can land on a GPU write-watch guard page
// and trip the recomp's fault handler into a retry storm that HARD-LOCKS the game
// (see safe_ld32). /proc/self/mem + pread returns an error on such pages instead
// of faulting, so the inventory walk can follow pointers safely.
#if defined(__linux__)
#define GE_SAFE_GUEST_READ 1
#include <fcntl.h>
#include <unistd.h>
#endif

// Desktop-only discovery memory scanner (see the GEMSCAN block below). Guarded so
// none of it compiles into the Android/shipping build.
#if defined(__linux__) && !defined(__ANDROID__)
#define GE_MEMSCAN 1
#include <cstdarg>
#include <cstdlib>
#include <ctime>
#include <string>
#include <vector>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#endif

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

// Fault-safe big-endian 32-bit guest read for the live path. Reads via
// /proc/self/mem so a guarded/unmapped page returns an error instead of faulting
// the process (a raw deref there trips the write-watch handler into an infinite
// retry that hard-locks the game). Returns false if the address is unreadable;
// used by the inventory-list walk, which follows pointers that could be stale.
#ifdef GE_SAFE_GUEST_READ
inline int bridge_memfd() {
  static int fd = ::open("/proc/self/mem", O_RDONLY | O_CLOEXEC);
  return fd;
}
inline bool safe_ld32(uint8_t* base, uint32_t ga, uint32_t* out) {
  uint8_t b[4];
  if (pread(bridge_memfd(), b, 4, (off_t)((uintptr_t)base + ga)) != 4) return false;
  *out = ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3];
  return true;
}
#else
inline bool safe_ld32(uint8_t* base, uint32_t ga, uint32_t* out) { *out = LD32(base, ga); return true; }
#endif

// A guest address is plausible if it is non-null and inside the 32-bit guest
// space. A Xenon title spreads allocations across several heaps (0x40000000,
// 0x82000000 module, 0x90000000, and the physical heaps at 0xA0000000/
// 0xC0000000/0xE0000000), so the old 0x82..0x90-only bound wrongly rejected the
// physical-heap player struct. We only exclude the low null page and the very
// top; the discovery scanner further restricts reads to committed pages via
// /proc/self/maps, and the live read path guards on a confirmed kPlayerPtrAddr.
inline bool plausible_guest_ptr(uint32_t ga) {
  return ga >= 0x00010000u && ga < 0xFFFF0000u;
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

// ---------------------------------------------------------------------------
// DIRECT (absolute-address) read path -- CONFIRMED ON DESKTOP 2026-07-02.
// The guest heap is deterministic: the equipped-weapon state block sits at a
// FIXED guest address every boot (verified identical across two app restarts),
// so we address it directly instead of chasing a pointer chain. These are the
// live equipped-weapon fields; the full per-weapon inventory array (reserve +
// held set) is not mapped yet, so this path publishes a one-weapon snapshot
// (the equipped weapon + its clip). 0 => direct path disabled.
inline constexpr uint32_t kEquipIdAddr   = 0x447f10b0u;  // 32-bit equipped weapon id
inline constexpr uint32_t kEquipIdAddr2  = 0x447f10c8u;  // 32-bit equipped id (agreement gate)
inline constexpr uint32_t kClipAddr      = 0x447f10f4u;  // 32-bit current-weapon clip
inline constexpr uint32_t kWeaponDefAddr = 0x447f10c0u;  // 32-bit per-weapon def ptr (in-game gate)

// Carried-weapons doubly-linked list. A fixed field in the player entity points
// at the head node; each 20-byte node is {flag, weapon id, 0xcccccccc, next,
// prev}. Walking `next` enumerates every carried weapon (the list is circular).
// Confirmed by a pickup diff (picking up a weapon adds a node with its id).
inline constexpr uint32_t kWeaponListHeadPtr = 0x447f1a2cu;  // -> head node
inline constexpr uint32_t kNodeIdOff         = 0x04u;        // node: 32-bit weapon id
inline constexpr uint32_t kNodeNextOff       = 0x0cu;        // node: 32-bit next ptr

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

// ===========================================================================
// GEMSCAN -- desktop discovery memory scanner (Cheat-Engine style).
//
// PURPOSE: find the guest layout constants at the top of this file empirically,
// on a desktop build with the game running, with no symbols / IDA / map. It scans
// the guest heap for a known value (e.g. your current ammo), then repeatedly
// narrows the surviving candidate set as that value changes in-game. When the set
// collapses to a handful of addresses you have found the ammo array; the player
// struct base and the pointer-to-it (kPlayerPtrAddr) fall out from there.
//
// DRIVING IT (desktop only, gated behind cvar ge_gamestate_diag):
//   The game thread polls a one-line command file once per frame and appends
//   results to an output file + the log. Defaults:
//       command file : $GE_SCAN_CMD  (default /tmp/ge_scan.cmd)
//       output file  : $GE_SCAN_OUT  (default /tmp/ge_scan.out)
//   Issue a command from another terminal, e.g.:
//       echo 'find 16 7'   > /tmp/ge_scan.cmd   # all u16 == 7 (a clip count)
//       echo 'dec'         > /tmp/ge_scan.cmd   # keep those that DROPPED since
//       echo 'next 6'      > /tmp/ge_scan.cmd   # keep those now == 6
//       echo 'list'        > /tmp/ge_scan.cmd   # dump survivors (addr = value)
//       echo 'ptr32 0x...' > /tmp/ge_scan.cmd   # find u32 pointers equal to addr
//       echo 'read 0x.. 32'> /tmp/ge_scan.cmd   # one-off read
//       echo 'reset'       > /tmp/ge_scan.cmd
//   Commands: find <16|32> <val> | next <val> | changed | same | dec | inc |
//             ptr32 <ga> | list [n] | read <ga> <16|32> | reset
//   Numbers accept decimal or 0x-hex. A fresh 'find'/'ptr32' reseeds the set;
//   next/changed/same/dec/inc filter the current set and re-baseline it.
// ===========================================================================
#ifdef GE_MEMSCAN
namespace memscan {

inline constexpr uint64_t kGuestSpan = 0x100000000ull;  // 4 GB 32-bit guest space at membase
inline constexpr size_t   kMaxCands = 16000000u;        // cap: ~128 MB of {ga,last} pairs

struct Cand { uint32_t ga; uint32_t last; };
std::vector<Cand> g_cands;
int g_width = 0;                                    // 2 or 4 (bytes) of the active set

// Invoke fn(glo, ghi) for each committed, readable guest sub-range, discovered
// from /proc/self/maps intersected with [membase, membase+4GB). This covers ALL
// heaps automatically and never touches an unmapped page (which would SIGSEGV).
template <typename Fn>
void for_each_committed(uint8_t* base, Fn&& fn) {
  const uintptr_t membase = (uintptr_t)base;
  const uintptr_t memend  = membase + kGuestSpan;
  FILE* m = std::fopen("/proc/self/maps", "r");
  if (!m) return;
  char line[512];
  while (std::fgets(line, sizeof(line), m)) {
    uintptr_t s, e; char perms[8] = {0};
    if (std::sscanf(line, "%lx-%lx %7s", &s, &e, perms) != 3) continue;
    if (perms[0] != 'r') continue;                 // readable only
    if (e <= membase || s >= memend) continue;     // outside the guest reservation
    uintptr_t rs = s < membase ? membase : s;
    uintptr_t re = e > memend ? memend : e;
    uint64_t glo = (uint64_t)(rs - membase);
    uint64_t ghi = (uint64_t)(re - membase);
    if (ghi > 0xFFFFFFFFull) ghi = 0xFFFFFFFFull;
    fn((uint32_t)glo, (uint32_t)ghi);
  }
  std::fclose(m);
}

const char* cmd_path() { const char* p = std::getenv("GE_SCAN_CMD"); return p ? p : "/tmp/ge_scan.cmd"; }
const char* out_path() { const char* p = std::getenv("GE_SCAN_OUT"); return p ? p : "/tmp/ge_scan.out"; }

// Big-endian read of width w (2 or 4) from a host-side byte buffer.
inline uint32_t rd_buf(const uint8_t* p, int w) {
  return w == 2 ? (uint32_t)((p[0] << 8) | p[1])
                : (uint32_t)((p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]);
}

// Shared read-only handle on our own address space. Reading guest memory through
// this (pread) fails gracefully on guarded/unmapped pages instead of tripping the
// recomp's SIGSEGV write-watch handler and crashing -- so EVERY scanner read goes
// through it, not just the bulk scan.
int g_memfd = -1;
int memfd() { if (g_memfd < 0) g_memfd = ::open("/proc/self/mem", O_RDONLY); return g_memfd; }

// Fault-safe single read; returns 0xffffffff sentinel if the page is unreadable.
inline uint32_t rd(uint8_t* base, uint32_t ga, int w) {
  uint8_t b[4] = {0};
  if (pread(memfd(), b, (size_t)w, (off_t)((uintptr_t)base + ga)) < w) return 0xffffffffu;
  return rd_buf(b, w);
}

// Emit one line to both the log and the append-mode output file.
void emit(FILE* out, const char* fmt, ...) {
  char buf[512];
  va_list ap; va_start(ap, fmt);
  std::vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  REXKRNL_INFO("GEMSCAN {}", buf);
  if (out) { std::fputs(buf, out); std::fputc('\n', out); }
}

// Scan guest range [glo, ghi) for `val` at width `w`, reading via /proc/self/mem
// so guarded/unmapped pages fail gracefully (a raw deref would SIGSEGV against
// the recomp's GPU write-watch handler). Appends matches to g_cands.
// Returns false if capped. `bad` accumulates KB that could not be read.
bool scan_range(int memfd, uint8_t* base, int w, uint32_t val,
                uint32_t glo, uint32_t ghi, uint64_t* bad) {
  const uint32_t step = (uint32_t)w;
  if (glo % step) glo += step - (glo % step);
  static std::vector<uint8_t> buf;
  const size_t kChunk = 1u << 20;  // 1 MB, multiple of 4 so no value straddles a boundary
  if (buf.size() < kChunk + 4) buf.resize(kChunk + 4);
  for (uint32_t ga = glo; (uint64_t)ga + step <= ghi; ) {
    size_t want = kChunk;
    if ((uint64_t)ga + want > ghi) want = (size_t)(ghi - ga);
    off_t off = (off_t)((uintptr_t)base + ga);
    ssize_t got = pread(memfd, buf.data(), want, off);
    if (got <= 0) { *bad += want / 1024u; ga += (uint32_t)want; continue; }  // unreadable: skip
    for (ssize_t i = 0; (size_t)i + step <= (size_t)got; i += step) {
      if (rd_buf(buf.data() + i, w) == val) {
        if (g_cands.size() >= kMaxCands) return false;
        g_cands.push_back({(uint32_t)(ga + i), val});
      }
    }
    ga += (uint32_t)got;
  }
  return true;
}

// Fresh scan for value `val` at width `w` (2 or 4). With lo/hi (both nonzero)
// scans only [lo,hi); otherwise sweeps every committed guest region.
void seed(uint8_t* base, int w, uint32_t val, uint32_t lo, uint32_t hi, FILE* out) {
  g_cands.clear();
  g_width = w;
  int memfd = ::open("/proc/self/mem", O_RDONLY);
  if (memfd < 0) { emit(out, "seed: cannot open /proc/self/mem"); return; }
  bool ok = true; uint64_t bad = 0; size_t regions = 0;
  if (lo && hi) {
    regions = 1;
    ok = scan_range(memfd, base, w, val, lo, hi, &bad);
  } else {
    for_each_committed(base, [&](uint32_t glo, uint32_t ghi) {
      if (!ok) return;
      ++regions;
      ok = scan_range(memfd, base, w, val, glo, ghi, &bad);
    });
  }
  ::close(memfd);
  emit(out, "seed w%d val=0x%x (%u) range=%#x..%#x: %zu candidates, %zu regions, %llu KB unreadable%s",
       w, val, val, lo, hi ? hi : 0xffffffffu, g_cands.size(), regions,
       (unsigned long long)bad, ok ? "" : " [CAPPED - narrow range or use a rarer value]");
}

// Seed the candidate set with EVERY aligned address in [lo,hi) and its current
// value (an "unknown initial value" scan). Follow with changed/unchanged/dec/inc
// across a game action (e.g. picking up a weapon) to isolate what moved. Bounded
// region keeps the set under kMaxCands.
void snapshot(uint8_t* base, uint32_t lo, uint32_t hi, int w, FILE* out) {
  g_cands.clear();
  g_width = w;
  const uint32_t step = (uint32_t)w;
  if (lo % step) lo += step - (lo % step);
  static std::vector<uint8_t> buf;
  const size_t kChunk = 1u << 20;
  if (buf.size() < kChunk + 4) buf.resize(kChunk + 4);
  bool capped = false; uint64_t bad = 0;
  for (uint32_t ga = lo; (uint64_t)ga + step <= hi && !capped; ) {
    size_t want = kChunk;
    if ((uint64_t)ga + want > hi) want = (size_t)(hi - ga);
    ssize_t got = pread(memfd(), buf.data(), want, (off_t)((uintptr_t)base + ga));
    if (got <= 0) { bad += want / 1024u; ga += (uint32_t)want; continue; }
    for (ssize_t i = 0; (size_t)i + step <= (size_t)got; i += step) {
      if (g_cands.size() >= kMaxCands) { capped = true; break; }
      g_cands.push_back({(uint32_t)(ga + i), rd_buf(buf.data() + i, w)});
    }
    ga += (uint32_t)got;
  }
  emit(out, "snapshot w%d %#x..%#x: %zu addrs, %llu KB unreadable%s", w, lo, hi,
       g_cands.size(), (unsigned long long)bad, capped ? " [CAPPED - shrink region]" : "");
}

// Print the committed guest regions (the memory map) so we know which heaps exist.
void regions(uint8_t* base, FILE* out) {
  size_t n = 0; uint64_t total = 0;
  for_each_committed(base, [&](uint32_t glo, uint32_t ghi) {
    ++n; total += (uint64_t)(ghi - glo);
    emit(out, "  region %#010x .. %#010x  (%u KB)", glo, ghi, (ghi - glo) / 1024u);
  });
  emit(out, "regions: %zu committed, %llu KB total", n, (unsigned long long)(total / 1024u));
}

// Filter the current set. mode: 0 ==val, 1 changed, 2 same, 3 dec, 4 inc.
// Re-baselines survivors' `last` to their current value.
void filter(uint8_t* base, int mode, uint32_t val, FILE* out) {
  if (g_cands.empty() || g_width == 0) { emit(out, "no active set -- 'find' first"); return; }
  std::vector<Cand> keep;
  keep.reserve(g_cands.size());
  for (auto& c : g_cands) {
    uint32_t cur = rd(base, c.ga, g_width);
    bool ok = false;
    switch (mode) {
      case 0: ok = (cur == val); break;
      case 1: ok = (cur != c.last); break;
      case 2: ok = (cur == c.last); break;
      case 3: ok = (cur < c.last); break;
      case 4: ok = (cur > c.last); break;
    }
    if (ok) keep.push_back({c.ga, cur});
  }
  g_cands.swap(keep);
  const char* names[] = {"next", "changed", "same", "dec", "inc"};
  emit(out, "%s: %zu candidates remain", names[mode], g_cands.size());
}

// On-demand hex window at any guest address -- read a located struct's layout
// (e.g. dump before/after switching weapons to spot the equipped-id offset).
void dump(uint8_t* base, uint32_t ga, uint32_t len, FILE* out) {
  if (!plausible_guest_ptr(ga)) { emit(out, "dump @%#010x rejected (implausible)", ga); return; }
  if (len > 256u) len = 256u;
  uint8_t win[256] = {0};
  ssize_t got = pread(memfd(), win, len, (off_t)((uintptr_t)base + ga));
  if (got <= 0) { emit(out, "dump @%#010x unreadable", ga); return; }
  char hex[256 * 3 + 1]; int o = 0;
  for (ssize_t i = 0; i < got && o + 3 < (int)sizeof(hex); i++)
    o += std::snprintf(hex + o, sizeof(hex) - o, "%02x ", win[i]);
  emit(out, "dump @%#010x +%zd: %s", ga, got, hex);
}

void list(uint8_t* base, int n, FILE* out) {
  if (g_cands.empty()) { emit(out, "empty set"); return; }
  int shown = 0;
  for (auto& c : g_cands) {
    if (shown++ >= n) break;
    uint32_t cur = rd(base, c.ga, g_width);
    emit(out, "  @%#010x = 0x%x (%u)", c.ga, cur, cur);
  }
  emit(out, "listed %d of %zu (w%d)", shown, g_cands.size(), g_width);
}

// Parse+run one command line. Returns nothing; all output goes to log/out file.
void dispatch(uint8_t* base, const char* line) {
  FILE* out = std::fopen(out_path(), "a");
  char verb[32] = {0};
  char a[64] = {0}, b[64] = {0}, c[64] = {0}, d[64] = {0};
  int nf = std::sscanf(line, "%31s %63s %63s %63s %63s", verb, a, b, c, d);
  auto num = [](const char* s) -> uint32_t {
    return (uint32_t)std::strtoul(s, nullptr, 0);
  };
  if (nf >= 1 && std::strcmp(verb, "reset") == 0) {
    g_cands.clear(); g_width = 0; emit(out, "reset");
  } else if (nf >= 3 && std::strcmp(verb, "find") == 0) {
    int w = (num(a) == 32) ? 4 : 2;
    uint32_t lo = nf >= 4 ? num(c) : 0, hi = nf >= 5 ? num(d) : 0;
    seed(base, w, num(b), lo, hi, out);
  } else if (nf >= 2 && std::strcmp(verb, "ptr32") == 0) {
    uint32_t lo = nf >= 3 ? num(b) : 0, hi = nf >= 4 ? num(c) : 0;
    seed(base, 4, num(a), lo, hi, out);    // pointers to a guest address
  } else if (nf >= 2 && std::strcmp(verb, "next") == 0) {
    filter(base, 0, num(a), out);
  } else if (nf >= 1 && std::strcmp(verb, "changed") == 0) {
    filter(base, 1, 0, out);
  } else if (nf >= 1 && std::strcmp(verb, "same") == 0) {
    filter(base, 2, 0, out);
  } else if (nf >= 1 && std::strcmp(verb, "dec") == 0) {
    filter(base, 3, 0, out);
  } else if (nf >= 1 && std::strcmp(verb, "inc") == 0) {
    filter(base, 4, 0, out);
  } else if (nf >= 1 && std::strcmp(verb, "list") == 0) {
    list(base, nf >= 2 ? (int)num(a) : 32, out);
  } else if (nf >= 3 && std::strcmp(verb, "read") == 0) {
    uint32_t ga = num(a); int w = (num(b) == 32) ? 4 : 2;
    if (plausible_guest_ptr(ga)) emit(out, "read @%#010x w%d = 0x%x", ga, w, rd(base, ga, w));
    else emit(out, "read @%#010x rejected (implausible)", ga);
  } else if (nf >= 4 && std::strcmp(verb, "write") == 0) {
    // TEMP discovery aid: poke a guest field live to test what triggers a weapon
    // switch. write <ga> <16|32> <val>. Big-endian store, same as the game.
    uint32_t ga = num(a); int w = (num(b) == 32) ? 4 : 2; uint32_t v = num(c);
    if (!plausible_guest_ptr(ga)) { emit(out, "write @%#010x rejected (implausible)", ga); }
    else { if (w == 4) ST32(base, ga, v); else ST16(base, ga, (uint16_t)v);
           emit(out, "write @%#010x w%d = 0x%x", ga, w, v); }
  } else if (nf >= 2 && std::strcmp(verb, "dump") == 0) {
    dump(base, num(a), nf >= 3 ? num(b) : 64u, out);
  } else if (nf >= 3 && std::strcmp(verb, "snapshot") == 0) {
    int w = (nf >= 4 && num(c) == 16) ? 2 : 4;
    snapshot(base, num(a), num(b), w, out);
  } else if (nf >= 1 && std::strcmp(verb, "regions") == 0) {
    regions(base, out);
  } else if (nf >= 1 && std::strcmp(verb, "snap") == 0) {
    WeaponSnapshot ss = GetWeaponSnapshot();
    int32_t ammo = (ss.equipped_id >= 0 && ss.equipped_id < kMaxWeaponSlots)
                       ? (int32_t)ss.ammo[ss.equipped_id] : -1;
    emit(out, "snap: valid=%d equipped_id=%d held_count=%d ammo[eq]=%d frame=%u",
         ss.valid, ss.equipped_id, ss.held_count, ammo, ss.frame);
  } else {
    emit(out, "?? '%s' (find <16|32> v [lo hi] | snapshot lo hi [16|32] | next v | changed|same|dec|inc | ptr32 ga [lo hi] | list [n] | read ga <16|32> | write ga <16|32> v | dump ga [len] | regions | snap | reset)", line);
  }
  if (out) std::fclose(out);
}

// Poll the command file each frame; run its contents when it changes.
void poll(uint8_t* base) {
  static time_t last_mtime = 0;
  static bool primed = false;
  struct stat st;
  if (::stat(cmd_path(), &st) != 0) return;
  if (primed && st.st_mtime == last_mtime) return;
  last_mtime = st.st_mtime;
  if (!primed) {                                    // first sighting: announce, don't run stale
    primed = true;
    REXKRNL_INFO("GEMSCAN ready: membase={} cmd={} out={}", (void*)base, cmd_path(), out_path());
    return;
  }
  FILE* f = std::fopen(cmd_path(), "r");
  if (!f) return;
  char line[256];
  if (std::fgets(line, sizeof(line), f)) {
    line[std::strcspn(line, "\r\n")] = 0;
    if (line[0]) dispatch(base, line);
  }
  std::fclose(f);
}

}  // namespace memscan
#endif  // GE_MEMSCAN

// --- read path: build a snapshot from guest memory --------------------------
WeaponSnapshot read_snapshot(uint8_t* base, uint32_t frame) {
  WeaponSnapshot s{};
  s.frame = frame;

  // DIRECT path (confirmed): the equipped-weapon block is at a fixed guest addr.
  // Publish a one-weapon snapshot (equipped weapon + its clip) until the full
  // per-weapon inventory array is mapped. Gated so we only publish valid=true
  // when the block clearly holds a live in-game weapon, not title-screen junk.
  if (kEquipIdAddr != 0u) {
    // EVERY read here is fault-safe (safe_ld32): the recomp's GPU write-watch
    // guard pages make a raw deref of a stray guest pointer trip an infinite
    // retry that hard-locks the game. If the entity can't be read, stay inert.
    uint32_t id_u, id2_u, clip, defp;
    if (!safe_ld32(base, kEquipIdAddr, &id_u) || !safe_ld32(base, kEquipIdAddr2, &id2_u) ||
        !safe_ld32(base, kClipAddr, &clip) || !safe_ld32(base, kWeaponDefAddr, &defp)) {
      return s;
    }
    int32_t id  = static_cast<int32_t>(id_u);
    int32_t id2 = static_cast<int32_t>(id2_u);

    // In-game gate: the two equipped-id copies agree, the id is a real slot, the
    // clip is sane, and the per-weapon def pointer points into the guest module.
    const bool in_game = (id == id2) && id >= 0 && id < kMaxWeaponSlots &&
                         clip <= 4000u &&
                         (defp >= 0x82000000u && defp < 0x84000000u);
    if (in_game) {
      s.equipped_id = id;

      // Walk the circular carried-weapons list from the head node, collecting
      // every held weapon id. safe_ld32 makes following the pointers fault-proof:
      // an unreadable/stale node just stops the walk (we fall back to equipped-only
      // below), so a corrupt list can never lock the game.
      uint32_t head = 0, node = 0, mask = 0;
      if (safe_ld32(base, kWeaponListHeadPtr, &head)) {
        node = head;
        for (int guard = 0; guard < kMaxWeaponSlots; ++guard) {
          uint32_t wid_u;
          if (!safe_ld32(base, node + kNodeIdOff, &wid_u)) break;
          int32_t wid = static_cast<int32_t>(wid_u);
          if (wid >= 0 && wid < kMaxWeaponSlots && !(mask & (1u << wid))) {
            mask |= (1u << wid);
            if (s.held_count < kMaxWeaponSlots) s.held_ids[s.held_count++] = static_cast<uint8_t>(wid);
          }
          uint32_t next;
          if (!safe_ld32(base, node + kNodeNextOff, &next) || next == head || next < 0x00010000u) break;
          node = next;
        }
      }

      // Make sure the equipped weapon is present even if the walk came up empty
      // (e.g. head pointer not yet valid this frame).
      if (!(mask & (1u << id))) {
        mask |= (1u << id);
        if (s.held_count < kMaxWeaponSlots) s.held_ids[s.held_count++] = static_cast<uint8_t>(id);
      }
      s.held_mask = mask;

      // Ammo: only the equipped weapon's clip is known so far (the per-weapon
      // reserve pool is not mapped yet), so other slots stay 0.
      s.ammo[id] = static_cast<uint16_t>(clip);
      s.valid = (s.held_count > 0);
    }
    return s;
  }

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

int32_t PeekEquipRequest() {
  uint32_t req = g_pending_equip.load(std::memory_order_acquire);
  return (req & kReqFlag) ? static_cast<int32_t>(req & ~kReqFlag) : kNoWeapon;
}

void ClearEquipRequest() {
  g_pending_equip.store(0, std::memory_order_release);
}

void OnFrame(void* ppc_ctx, uint8_t* guest_base) {
  if (!guest_base) return;
  uint32_t frame = g_frame.fetch_add(1, std::memory_order_relaxed) + 1;

  run_probe(guest_base, frame);

#ifdef GE_MEMSCAN
  if (REXCVAR_GET(ge_gamestate_diag)) memscan::poll(guest_base);
#endif

  WeaponSnapshot s = read_snapshot(guest_base, frame);
  publish(s);
  (void)ppc_ctx;  // reserved: equip actuation now happens via native-Y injection
                  // in ge_hooks.cpp, not a guest-memory write from this frame hook.
}

}  // namespace ge::gamestate
