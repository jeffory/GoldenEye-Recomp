// ge - project mid-ASM hooks: 0x830E0xxx fragment reconstruction.
#include <thread>
#include <algorithm>  // std::min/std::max (ge_mouse_camera aim/sway clamps)
#include <cmath>      // sqrtf (ge_mouse_camera crosshair distance)
//
// 8 functions branch to 0x830E0xxx, ZERO in the static XEX (rexglue codegen
// stubs it) but at runtime real PPC code (identical to fragments IDA
// mis-coalesced into sub_821A9720). codegen prunes the code after the
// unconditional `b 0x830E0xxx`, so each continuation point is declared as its
// own ge_cont_* function. Each [[midasm_hook]] (return = true) replicates the
// fragment's register/memory effect, tail-invokes the continuation function,
// and the recompiled source function then returns.

#include <atomic>
#include <cstdint>
#include <cstring>

#include "ge_init.h"   // PPCRegister/PPCContext + generated function decls
#include "ge_fps.h"    // ge::FpsOnFrame (guest-FPS benchmark recorder)
#include "ge_gamestate.h"  // ge::gamestate::OnFrame (game-state bridge pump)
#include "ge_dualscreen.h"  // ge::DualScreen (second-screen weapon menu pacing)
#include "ge_touchpad.h"    // ge::TouchPad (on-screen touch controls -> guest pad)
#include <rex/cvar.h>  // REXCVAR_DEFINE_BOOL / REXCVAR_GET (ge_freeze_diag)
#include <rex/perf/counter.h>  // rex::perf frame-stage counters (GESPIKE)
#include <rex/hook.h>  // ThreadState, kernel_state, memory
#include <rex/runtime.h>
#include <rex/system/fault_diag.h>  // write-watch fault diagnostics (GEWATCHDOG dump)
#include <rex/system/xmemory.h>
#include <rex/graphics/graphics_system.h>
#include <rex/graphics/command_processor.h>
#include <rex/system/xthread.h>
#include <rex/system/kernel_state.h>
#include <rex/ui/keybinds.h>      // ParseVirtualKey (keyboard rebinding)
#include <rex/ui/virtual_key.h>
#include <rex/ui/window.h>        // mouse capture / cursor / warp (cross-platform)
#include <rex/ui/window_listener.h>
#include <rex/ui/ui_event.h>
#include <cstdio>
#include <mutex>
#include <string>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>  // ShellExecuteW (WIN32_LEAN_AND_MEAN excludes it)
#endif

// Freeze watchdog + per-stall pipeline diagnostics. These were always-on while
// the frame-pacing freeze was being diagnosed: a 250ms-polling watchdog thread
// that, on a stall, walks every guest thread's stack (probing page readability
// via /dev/null on POSIX) and emits heavy logs, plus a per-present heartbeat.
// They cost CPU every frame and have no place in a shipping build, so they are
// gated behind this debug cvar (default OFF). Flip it on to investigate a stall.
REXCVAR_DEFINE_BOOL(ge_freeze_diag, false, "GPU",
                    "GoldenEye: enable the freeze watchdog + per-stall pipeline diagnostics "
                    "(heavy logging + guest-thread stack walks; OFF in shipping builds)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace ge {
// Relaunch this same executable as a fresh, detached process. Used by the ONLINE
// pause-menu tab's "Save & Restart": the new instance reads the just-written
// ge.toml (new username / server / online-enable) at boot, then the caller tears
// the current process down. Launching a second instance of a running exe is fine
// on Windows -- the image file is opened share-read.
#if defined(_WIN32)
void LaunchSelfDetached() {
  wchar_t exe_path[MAX_PATH];
  DWORD n = GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
  if (n == 0 || n >= MAX_PATH) {
    return;  // can't resolve our own path; skip relaunch (caller still quits)
  }
  // Start it from the exe's own directory so a normal boot's relative paths hold.
  std::wstring full(exe_path, exe_path + n);
  size_t slash = full.find_last_of(L"\\/");
  std::wstring workdir = (slash == std::wstring::npos) ? std::wstring() : full.substr(0, slash);
  ShellExecuteW(nullptr, L"open", exe_path, nullptr,
                workdir.empty() ? nullptr : workdir.c_str(), SW_SHOWNORMAL);
}
#else
// Non-Windows (incl. Android): no in-process relaunch. "Save & Restart" still
// writes the new config; the player relaunches the app manually. An Android
// auto-restart would require an Activity recreate via JNI - out of scope here.
void LaunchSelfDetached() {}
#endif
}  // namespace ge

#if !defined(_WIN32)
#include <fcntl.h>
#include <unistd.h>
#endif

namespace {
// Returns a pointer p <= end <= p+want such that [p, end) is safely readable,
// or p (empty) if p itself is not. Lets the freeze watchdog walk the guest stack
// without faulting on uncommitted / guard pages. Windows uses VirtualQuery;
// POSIX (incl. Android) probes readability by write()ing each page to /dev/null,
// which returns EFAULT for an unreadable address instead of faulting.
const uint8_t* ge_readable_end(const uint8_t* p, size_t want) {
#if defined(_WIN32)
  MEMORY_BASIC_INFORMATION mbi;
  if (VirtualQuery(p, &mbi, sizeof(mbi)) != sizeof(mbi) || mbi.State != MEM_COMMIT ||
      (mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                      PAGE_EXECUTE_READWRITE)) == 0) {
    return p;
  }
  const uint8_t* rend = static_cast<const uint8_t*>(mbi.BaseAddress) + mbi.RegionSize;
  const uint8_t* send = p + want;
  return send <= rend ? send : rend;
#else
  static int null_fd = ::open("/dev/null", O_WRONLY | O_CLOEXEC);
  if (null_fd < 0) {
    return p;
  }
  constexpr uintptr_t kPage = 4096;
  const uint8_t* end = p + want;
  const uint8_t* readable_to = p;
  for (const uint8_t* pg = reinterpret_cast<const uint8_t*>(
           reinterpret_cast<uintptr_t>(p) & ~(kPage - 1));
       pg < end; pg += kPage) {
    if (::write(null_fd, pg, 1) != 1) {
      break;  // EFAULT: page not readable
    }
    readable_to = pg + kPage;
  }
  if (readable_to <= p) {
    return p;
  }
  return readable_to < end ? readable_to : end;
#endif
}
}  // namespace

// Probe to read CommandProcessor's protected ring read pointer (legal: a
// derived class may touch protected base members; we only reinterpret an
// existing CP* and call a non-virtual accessor -- no construction, no
// vtable use, layout-compatible single inheritance).
namespace {
struct CPProbe : rex::graphics::CommandProcessor {
  uint32_t rpi() const { return read_ptr_index_; }
  uint32_t wpi() const { return write_ptr_index_.load(std::memory_order_acquire); }
};
// rexglue CP swap counter sampled at the last guest present (sub_821996F8).
// "GPU finished the just-submitted frame" == counter advanced past this.
std::atomic<uint32_t> g_present_cpcnt{0};
// Guest tick at the last present, for a bounded completion wait.
std::atomic<uint32_t> g_present_tb{0};
// Count of 80ms GPU-wait skip-bit fallback EPISODES in ge_dbg_now (one per stall
// stretch, not per poll). Logged in the GEWATCHDOG STALL dump so a captured
// lock-up shows whether the render-side skip-bit fallback fired.
std::atomic<uint32_t> g_skipbit_fallback_fires{0};
inline rex::graphics::CommandProcessor* ge_cp() {
  auto* ks = rex::system::kernel_state();
  if (!ks) return nullptr;
  auto* rt = ks->emulator();
  if (!rt) return nullptr;
  auto* igs = rt->graphics_system();
  if (!igs) return nullptr;
  return static_cast<rex::graphics::GraphicsSystem*>(igs)->command_processor();
}
inline rex::graphics::GraphicsSystem* ge_gs() {
  auto* ks = rex::system::kernel_state();
  if (!ks) return nullptr;
  auto* rt = ks->emulator();
  if (!rt) return nullptr;
  auto* igs = rt->graphics_system();
  if (!igs) return nullptr;
  return static_cast<rex::graphics::GraphicsSystem*>(igs);
}
}  // namespace

namespace {
inline void getcb(PPCContext*& ctx, uint8_t*& base) {
  ctx = rex::runtime::ThreadState::Get()->context();
  base = rex::system::kernel_state()->memory()->virtual_membase();
}
inline uint32_t LD32(uint8_t* b, uint32_t ga) {
  uint32_t v; std::memcpy(&v, b + ga, 4); return __builtin_bswap32(v);
}
inline uint64_t LD64(uint8_t* b, uint32_t ga) {
  uint64_t v; std::memcpy(&v, b + ga, 8); return __builtin_bswap64(v);
}
inline void ST32(uint8_t* b, uint32_t ga, uint32_t val) {
  uint32_t v = __builtin_bswap32(val); std::memcpy(b + ga, &v, 4);
}
inline void STF32(uint8_t* b, uint32_t ga, float f) {
  uint32_t v; std::memcpy(&v, &f, 4); v = __builtin_bswap32(v);
  std::memcpy(b + ga, &v, 4);
}
inline float LDF32(uint8_t* b, uint32_t ga) {
  uint32_t v; std::memcpy(&v, b + ga, 4); v = __builtin_bswap32(v);
  float f; std::memcpy(&f, &v, 4); return f;
}
inline uint16_t LD16(uint8_t* b, uint32_t ga) {
  uint16_t v; std::memcpy(&v, b + ga, 2); return __builtin_bswap16(v);
}
inline void ST16(uint8_t* b, uint32_t ga, uint16_t val) {
  uint16_t v = __builtin_bswap16(val); std::memcpy(b + ga, &v, 2);
}
}  // namespace

// rexglue CP WAIT_REG_MEM >60ms deadlock-breaker fire count (command_processor.cpp).
// Lets the watchdog report whether the CPU<->GPU fence breaker fired in a lock-up.
extern "C" uint32_t rex_ge_cp_wait_reg_mem_timeouts();
// rexglue CP drain-progress sequence (command_processor.cpp). Stagnant while the
// ring is non-empty = the CP worker isn't getting scheduled (starvation).
extern "C" uint64_t rex_ge_cp_progress_seq();

// Discovery diagnostic toggle, defined in ge_gamestate.cpp. Declared here so
// ge_dbg_weapon_apply (below) can gate its logging without a header dependency.
REXCVAR_DECLARE(bool, ge_gamestate_diag);

// CP-starvation episodes observed by the watchdog (ring non-empty but the CP
// progress seq did not advance across a 250ms watchdog tick). Coarse by design
// -- fine-grained starvation shows up as missing time in the per-frame CP
// counters instead. Read by the GESPIKE log line (ge_fps.cpp).
std::atomic<uint64_t> g_cp_starved_episodes{0};
namespace ge {
uint64_t GetCpStarvedEpisodes() {
  return g_cp_starved_episodes.load(std::memory_order_relaxed);
}
}  // namespace ge

// ===========================================================================
// Freeze watchdog. Auto-detects the visual freeze (the guest keeps presenting
// -- present# advancing -- but the GPU command ring stops advancing) and logs
// the exact pipeline state ONCE per stall episode, so we can read the mechanism
// off the log instead of capturing a live process. Zero gameplay effect.
// ===========================================================================
namespace {
std::atomic<uint32_t> g_ge_device{0};   // device struct (dev) seen by ge_dbg_now
std::atomic<uint32_t> g_ge_idblk{0};    // id-block (idblk) seen by ge_dbg_now
std::atomic<uint32_t> g_dbgnow_calls{0};  // increments each ge_dbg_now (guest polling sub_82198C28)

// Decode the SDK's write-watch fault diagnostics into ge.log. Called from the
// watchdog thread (normal context - logging is fine here). `full` also dumps
// the event ring; the summary line alone is cheap enough for periodic use.
void ge_dump_fault_diag(bool full) {
  auto& d = rex::system::fault_diag();
  REXKRNL_INFO(
      "GEWWDIAG wwf={} early_abort={} cb_handled={} cb_unhandled={} recovered={} "
      "recovery_fatal={} vetoes={} any_watched_false={} rearm={} protect_ro_watched={} "
      "shadow_disagree={} failsafe={} consec_max={}",
      d.ww_faults_total.load(), d.early_abort_hits.load(), d.cb_handled.load(),
      d.cb_unhandled.load(), d.recovered.load(), d.recovery_fatal.load(),
      d.unprotect_vetoes.load(), d.any_watched_false.load(), d.rearm_races.load(),
      d.protect_ro_watched.load(), d.shadow_disagreements.load(), d.failsafe_fires.load(),
      d.consecutive_max.load());
  if (!full) return;
  static const char* kPathNames[] = {
      "none",      "early_abort", "cb_handled",   "cb_unhandled", "recovered",
      "rec_fatal", "rec_lied",    "veto",         "no_watch",     "rearm",
      "protect_ro", "failsafe",   "shadow_lied"};
  // Oldest-first up to the ring size; skip never-written slots.
  uint32_t next = d.ring_next.load(std::memory_order_relaxed);
  for (uint32_t k = 0; k < rex::system::FaultDiag::kRingSize; ++k) {
    const auto& r = d.ring[(next + k) & (rex::system::FaultDiag::kRingSize - 1)];
    if (r.path == 0) continue;
    const char* name =
        r.path < sizeof(kPathNames) / sizeof(kPathNames[0]) ? kPathNames[r.path] : "?";
    REXKRNL_INFO(
        "GEWWRING seq={} t={}.{:06}s path={} host={:#x} guest={:#010x} pc={:#x} heap={:#010x} "
        "gpage={} cur={} state={} detail={}",
        r.fault_seq, r.timestamp_ns / 1000000000ull, (r.timestamp_ns % 1000000000ull) / 1000,
        name, r.host_addr, r.guest_vaddr, r.pc, r.heap_base, r.guest_page, r.current_protect,
        r.page_state, r.detail);
  }
}

void ge_watchdog_thread() {
  uint8_t* base = rex::system::kernel_state()->memory()->virtual_membase();
  uint32_t last_wpi = 0xFFFFFFFFu, last_rpi = 0, last_present = 0, last_submit = 0;
  uint32_t present_at_stall_start = 0, dbg_at_stall_start = 0, submit_at_stall_start = 0;
  uint32_t stall = 0;
  uint64_t last_cp_seq = 0;
  bool logged = false;
  bool recover_fired = false;
  for (;;) {
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    auto* cp = ge_cp();
    if (!cp) continue;
    uint32_t wpi = static_cast<CPProbe*>(cp)->wpi();
    uint32_t rpi = static_cast<CPProbe*>(cp)->rpi();

    // CP starvation: work queued (ring non-empty) but the CP's drain-progress
    // seq didn't move across a whole 250ms tick -> the worker isn't being
    // scheduled. One episode per stagnant tick; surfaced in GESPIKE.
    uint64_t cp_seq = rex_ge_cp_progress_seq();
    if (rpi != wpi && cp_seq == last_cp_seq) {
      g_cp_starved_episodes.fetch_add(1, std::memory_order_relaxed);
    }
    last_cp_seq = cp_seq;

    // Audio-emitter pool occupancy (GEEFFPOOL). The 200x32B X3DAudio emitter
    // pool at 0x83064E00 (allocator sub_82144058: slot word0 == 0 means free)
    // is the suspected "sound drops after minutes of play" culprit: once it
    // pins at 200/200, every new positional sound silently fails to spawn --
    // and the spawner's unchecked NULL write is what fires GEWWFAILSAFE.
    // Sample used slots each tick: WARN on the full transition, note the
    // recovery, and heartbeat 1/5s so a session log shows occupancy vs time.
    {
      static uint32_t pool_prev_used = 0;
      static int pool_log_div = 0;
      uint32_t pool_used = 0;
      for (uint32_t s = 0; s < 200u; ++s) {
        if (LD32(base, 0x83064E00u + s * 32u) != 0u) ++pool_used;
      }
      if (pool_used == 200u && pool_prev_used < 200u) {
        REXKRNL_WARN("GEEFFPOOL FULL 200/200 - new positional sounds drop until slots free");
      } else if (pool_prev_used == 200u && pool_used < 200u) {
        REXKRNL_INFO("GEEFFPOOL recovered {}/200", pool_used);
      }
      pool_prev_used = pool_used;
      if (++pool_log_div >= 20) {  // ~5s at the 250ms tick
        pool_log_div = 0;
        REXKRNL_INFO("GEEFFPOOL used={}/200", pool_used);
      }
    }

    uint32_t present = g_present_cpcnt.load(std::memory_order_relaxed);
    uint32_t dbg = g_dbgnow_calls.load(std::memory_order_relaxed);
    uint32_t dev = g_ge_device.load(std::memory_order_relaxed);
    uint32_t idblk = g_ge_idblk.load(std::memory_order_relaxed);
    uint32_t submit = dev ? LD32(base, dev + 16544) : 0;

    // TOTAL-FREEZE trigger. The original stall trigger below requires presents
    // to stay ALIVE while the ring freezes -- a real 23-minute-session lockup
    // (guest wedged in a SIGSEGV/maps-parse storm) stopped present# too and
    // the watchdog stayed silent by design. Catch the everything-stopped case:
    // the guest was clearly booted (dev known, submit>0) but neither submit
    // nor present# moved for ~3s. Log once per episode with the state that
    // identified the last one, plus the on-attach recipe.
    {
      static uint32_t tf_last_submit = 0, tf_last_present = 0, tf_ticks = 0;
      static bool tf_logged = false;
      if (dev && submit != 0 && submit == tf_last_submit && present == tf_last_present) {
        if (++tf_ticks >= 12 && !tf_logged) {  // 12 x 250ms = ~3s of nothing
          tf_logged = true;
          uint32_t presented = LD32(base, dev + 16552);
          uint32_t rg = LD32(base, 0x8242043Cu);
          REXKRNL_INFO(
              "GEWATCHDOG TOTAL-FREEZE: no submit/present progress ~3s | submit={} presented={} "
              "present#={} | ring rpi={:#x} wpi={:#x} [{}] | cp_seq={} dbgnow_polls={} | "
              "render-gate={} | skipbit-fires={} wrm-timeouts={} starved={}",
              submit, presented, present, rpi, wpi, (rpi == wpi ? "DRAINED" : "PENDING"), cp_seq,
              dbg, rg, g_skipbit_fallback_fires.load(std::memory_order_relaxed),
              rex_ge_cp_wait_reg_mem_timeouts(),
              g_cp_starved_episodes.load(std::memory_order_relaxed));
          REXKRNL_INFO(
              "GEWATCHDOG TOTAL-FREEZE: last-frame stages cpexec={}us cpidle={}us wrm={}us "
              "present={}us gwait={}us gpu={}us strans={}us pcomp={}us texup={}us gio={}us "
              "| if a guest thread is burning CPU, attach with: "
              "run-as com.sunjaycy.goldeneye simpleperf record -g -p <pid>",
              rex::perf::GetSnapshotCounter(rex::perf::CounterId::kCpExecuteUs),
              rex::perf::GetSnapshotCounter(rex::perf::CounterId::kCpIdleUs),
              rex::perf::GetSnapshotCounter(rex::perf::CounterId::kCpWaitRegMemUs),
              rex::perf::GetSnapshotCounter(rex::perf::CounterId::kPresentBlockUs),
              rex::perf::GetSnapshotCounter(rex::perf::CounterId::kGuestGpuWaitUs),
              rex::perf::GetSnapshotCounter(rex::perf::CounterId::kGpuFrameUs),
              rex::perf::GetSnapshotCounter(rex::perf::CounterId::kShaderTranslateUs),
              rex::perf::GetSnapshotCounter(rex::perf::CounterId::kPipelineCompileUs),
              rex::perf::GetSnapshotCounter(rex::perf::CounterId::kTextureUploadUs),
              rex::perf::GetSnapshotCounter(rex::perf::CounterId::kGuestFileIoUs));
          // Full write-watch fault anatomy: if a guest thread is wedged in a
          // fault loop, the ring holds the decisions of its last iterations.
          ge_dump_fault_diag(/*full=*/true);
        }
      } else {
        tf_ticks = 0;
        tf_logged = false;
      }
      tf_last_submit = submit;
      tf_last_present = present;
    }

    // Write-watch anomaly watcher: the failsafe, unprotect vetoes, shadow
    // disagreements and recovery events should all be zero in a healthy
    // session. Log (rate-limited) the moment any of them move so the event is
    // timestamped against GESPIKE/GESHOWN context even when no freeze follows.
    {
      auto& wwd = rex::system::fault_diag();
      static uint64_t ww_last_failsafe = 0, ww_last_anom = 0, ww_last_recovered = 0;
      static uint32_t ww_cooldown = 0;
      if (ww_cooldown) --ww_cooldown;
      uint64_t failsafe = wwd.failsafe_fires.load(std::memory_order_relaxed);
      uint64_t anom = wwd.unprotect_vetoes.load(std::memory_order_relaxed) +
                      wwd.shadow_disagreements.load(std::memory_order_relaxed) +
                      wwd.rearm_races.load(std::memory_order_relaxed) +
                      wwd.protect_ro_watched.load(std::memory_order_relaxed);
      uint64_t recov = wwd.recovered.load(std::memory_order_relaxed) +
                       wwd.recovery_fatal.load(std::memory_order_relaxed);
      if (failsafe != ww_last_failsafe) {
        REXKRNL_INFO("GEWWFAILSAFE fired (total={}) - a fault-looping page was force-unprotected",
                     failsafe);
        ge_dump_fault_diag(/*full=*/true);
        ww_last_failsafe = failsafe;
        ww_last_anom = anom;
        ww_last_recovered = recov;
      } else if ((anom != ww_last_anom || recov != ww_last_recovered) && ww_cooldown == 0) {
        ge_dump_fault_diag(/*full=*/false);
        ww_cooldown = 20;  // at most one summary per ~5s
        ww_last_anom = anom;
        ww_last_recovered = recov;
      }
    }

    bool present_alive = (present != last_present);
    bool ring_moved = (wpi != last_wpi) || (rpi != last_rpi);
    if (present_alive && !ring_moved) {
      if (stall == 0) {
        present_at_stall_start = present;
        dbg_at_stall_start = dbg;
        submit_at_stall_start = submit;
        recover_fired = false;
      }
      ++stall;
      // AUTO-RECOVERY for the CPU<->GPU semaphore deadlock. The CP parks in a
      // WAIT_REG_MEM polling idblk for ==0 (the semaphore the render writes 0 to
      // release the CP). The render is parked waiting on GPU completion ->
      // deadlock. Write 0 to release the CP: it drains the buffer, delivers the
      // completion, the render resumes. Memory-only, no interrupt (safe).
      // Active recovery WRITES guest memory, so it is opt-in via ge_freeze_diag;
      // the default-on watchdog only observes + logs.
      if (stall >= 2 && dev && idblk && idblk < 0xFFFFFFFEu && REXCVAR_GET(ge_freeze_diag)) {
        ST32(base, idblk, 0u);  // release the CP's WAIT_REG_MEM semaphore
        if (!recover_fired) {
          recover_fired = true;
          REXKRNL_INFO("GEWATCHDOG RECOVERY: released CP semaphore (idblk={:#x} := 0)", idblk);
        }
      }
      if (stall >= 6 && !logged) {  // ~1.5s of present-but-no-ring
        logged = true;
        uint32_t presented = dev ? LD32(base, dev + 16552) : 0;
        uint32_t target = dev ? LD32(base, dev + 10908) : 0;
        // idblk+0 is the WAIT_REG_MEM CPU<->GPU semaphore (the render writes 0 to
        // RELEASE the CP), NOT a completion counter -- so a value of 0 is the
        // normal released state. Report it as "semaphore=" and never infer "GPU
        // behind" from it (the old "submit > completed" test compared a frame
        // count against this semaphore and so trivially read "GPU BEHIND").
        uint32_t semaphore = idblk ? LD32(base, idblk + 0) : 0;
        uint32_t skip = dev ? (base[dev + 10941] & 2) : 0;
        REXKRNL_INFO(
            "GEWATCHDOG STALL: ring rpi={:#x} wpi={:#x} [{}] | present#={} (+{}/stall) | "
            "dbgnow_polls={} (+{}/stall) | submit={} presented={} target={} semaphore(idblk+0)={} "
            "skipbit={} | dev={:#x} idblk={:#x}",
            rpi, wpi, (rpi == wpi ? "DRAINED" : "PENDING"), present, present - present_at_stall_start,
            dbg, dbg - dbg_at_stall_start, submit, presented, target, semaphore, skip, dev, idblk);
        // Residual render-path stall mechanisms (cumulative fire counts): which one
        // tripped tells us whether this lock-up is the CPU<->GPU fence deadlock
        // (WAIT_REG_MEM breaker) or the guest GPU-wait giving up (skip-bit fallback).
        REXKRNL_INFO(
            "GEWATCHDOG -> residual fires: skipbit-fallback(80ms)={} | WAIT_REG_MEM-breaker(60ms)={}",
            g_skipbit_fallback_fires.load(std::memory_order_relaxed),
            rex_ge_cp_wait_reg_mem_timeouts());
        REXKRNL_INFO(
            "GEWATCHDOG -> render={} | presenting={} | producer={} | polling={}",
            // The real freeze mechanism: the guest latched the skip bit and is
            // bypassing the GPU-completion wait, so render is skipped every frame.
            (skip ? "SKIP-BIT LATCHED (render skipped every frame)" : "skip-bit clear"),
            (submit > presented ? "frames NOT presenting" : "caught up"),
            (submit != submit_at_stall_start ? "ALIVE (submitting)" : "STALLED (not submitting)"),
            (dbg != dbg_at_stall_start ? "guest spinning in sub_82198C28" : "guest NOT polling"));
        // Render gate: frame loop runs render+present only when dword_8242043C&2
        // (sub_8209E1C0). Set by sub_8209E1D0(mode): mode 3 at init (enabled),
        // mode 1 = bit clear = render skipped every frame = freeze.
        uint32_t rg = LD32(base, 0x8242043Cu);
        REXKRNL_INFO("GEWATCHDOG -> render-gate dword_8242043C={} -> render+present {}", rg,
                     (rg & 2u) ? "ENABLED" : "DISABLED (frame loop skips render = FREEZE)");
        // Device flags gating the present/submit (a1 = dev). +21516 != 0 => the
        // present SKIPS VdSwap (no screen update) and sub_821A4D50 takes its alt
        // path; +22280&4 gates the GPU-completion wait; +10941/+10943 = skip bits.
        if (dev) {
          uint32_t f21516 = LD32(base, dev + 21516u);
          uint32_t f22280 = LD32(base, dev + 22280u);
          uint32_t f22276 = LD32(base, dev + 22276u);
          uint32_t f21604 = LD32(base, dev + 21604u);
          uint32_t f21600 = LD32(base, dev + 21600u);
          uint32_t b10941 = base[dev + 10941u];
          uint32_t b10943 = base[dev + 10943u];
          REXKRNL_INFO(
              "GEWATCHDOG -> devflags +21516(VdSwap-skip if !=0)={:#x} | +22280&4(gpu-wait)={} | "
              "+22276={:#x} | +21604={} +21600={} (ring) | +10941={:#x} +10943={:#x}",
              f21516, (f22280 & 4u), f22276, f21604, f21600, b10941, b10943);
          uint32_t vbl = LD32(base, dev + 16532u);   // ctx[4133] vblank count
          uint32_t fr = LD32(base, dev + 16684u);    // ctx[4171] fence read idx
          uint32_t fw = LD32(base, dev + 16688u);    // ctx[4172] fence write idx
          REXKRNL_INFO("GEWATCHDOG -> vblank ctx[4133]={} | GPU fences read={} write={} [{}]", vbl,
                       fr, fw, (fr != fw ? "PENDING -- fences NOT retiring" : "drained"));
        }
        // Frame counter dword_8308851C is updated each frame AFTER the frame-
        // limiter (0x82189e64). Sample it twice: if FROZEN, the main thread never
        // exits the frame-limiter (clock/timebase not advancing for it); if it
        // ADVANCES, the main thread cycles and the render is skipped after.
        uint32_t fc1 = LD32(base, 0x8308851Cu);
        uint32_t tb1 = (uint32_t)REX_QUERY_TIMEBASE();
        std::this_thread::sleep_for(std::chrono::milliseconds(120));
        uint32_t fc2 = LD32(base, 0x8308851Cu);
        uint32_t tb2 = (uint32_t)REX_QUERY_TIMEBASE();
        REXKRNL_INFO(
            "GEWATCHDOG -> frameCounter 0x8308851C {}->{} [{}] | guestTimebase {}->{} [{}]", fc1, fc2,
            (fc1 != fc2 ? "ADVANCING (main thread cycles; render skipped after limiter)"
                        : "FROZEN (main thread STUCK in frame-limiter)"),
            tb1, tb2, (tb1 != tb2 ? "advancing" : "FROZEN"));
        // Dump every guest thread's jump state -- find WHERE the render workers
        // (guest entry 0x821A4A68) are wedged inside sub_821A4750. lr = return
        // addr, ctr = next indirect target, lastIndTgt = last REX_CALL_INDIRECT
        // target, msr bit 0x8000 = interrupts enabled.
        auto* ks2 = rex::system::kernel_state();
        if (ks2) {
          auto threads = ks2->object_table()->GetObjectsByType<rex::system::XThread>();
          for (auto& th : threads) {
            if (!th) continue;
            auto* ts = th->thread_state();
            if (!ts) continue;
            auto* c = ts->context();
            if (!c) continue;
            uint32_t sa = th->creation_params()->start_address;
            bool rw = (sa == 0x821A4A68u);
            REXKRNL_INFO(
                "GEWATCHDOG THREAD start={:#x}{} lr={:#x} ctr={:#x} lastIndTgt={:#x} msr={:#x} | "
                "r3={:#x} r11={:#x} r28={:#x} r29={:#x} r30={:#x} r31={:#x}",
                sa, rw ? " [RENDER-WORKER]" : "", (uint32_t)c->lr, c->ctr.u32,
                c->last_indirect_target, c->msr, c->r3.u32, c->r11.u32, c->r28.u32, c->r29.u32,
                c->r30.u32, c->r31.u32);
            // Guest stack walk: scan [r1, r1+0x2400) for guest code addresses
            // (0x82xxxxxx return addresses) -> the call chain, directly readable.
            {
              uint32_t sp = c->r1.u32;
              if (sp >= 0x10000u && sp < 0xC0000000u) {
                uint8_t* hsp = base + sp;
                const uint8_t* send = ge_readable_end(hsp, 0x2400u);
                if (send > hsp) {
                  char sbuf[500];
                  int soff = 0;
                  sbuf[0] = 0;
                  for (uint8_t* pp = hsp; pp + 4 <= send && soff < 460; pp += 4) {
                    uint32_t val;
                    std::memcpy(&val, pp, 4);
                    val = __builtin_bswap32(val);
                    if (val >= 0x82000000u && val < 0x84000000u) {
                      int n = std::snprintf(sbuf + soff, sizeof(sbuf) - soff, "%x ", val);
                      if (n > 0) soff += n;
                    }
                  }
                  REXKRNL_INFO("GEWATCHDOG   STACK start={:#x} sp={:#x}: {}", sa, sp, sbuf);
                }
              }
            }
            if (rw) {
              // a1 (worker struct) = r28; event = a1[2] = a1+0x20 (= r29);
              // queue = a1[3]: Flink/submit = a1+0x38, Blink/processed = a1+0x3C.
              // wait is INFINITE when a1->SignalState(a1+4) != *(v3+368), v3 = *a1.
              uint32_t bw = c->r28.u32;
              auto sLD = [&](uint32_t ga) -> uint32_t {
                return (ga >= 0x1000u && ga < 0x50000000u) ? LD32(base, ga) : 0xDEADBEEFu;
              };
              uint32_t v3 = sLD(bw);
              uint32_t sig = sLD(bw + 4);
              uint32_t v3f = sLD(v3 + 368);
              uint32_t subq = sLD(bw + 0x38);
              uint32_t procq = sLD(bw + 0x3C);
              REXKRNL_INFO(
                  "GEWATCHDOG   WORKER a1={:#x} queue Flink/submit={} Blink/proc={} [{}] | "
                  "SignalState={} v3={:#x} *(v3+368)={} -> wait={}",
                  bw, subq, procq,
                  (subq == procq ? "EMPTY (producer stopped feeding)" : "PENDING (LOST WAKEUP!)"),
                  sig, v3, v3f, (sig != v3f ? "INFINITE" : "30ms-timeout"));
            }
          }
          // Rapid-sample the main game thread (start 0x8235e4a8): it spends most
          // time in the frame-limiter, so one snapshot misses the render path.
          // Sample lr many times (yielding so it keeps running) -> the set of
          // unique guest PCs = its per-frame code path, revealing which render
          // subsystem call it reaches/skips.
          for (auto& th : threads) {
            if (!th) continue;
            if (th->creation_params()->start_address != 0x8235E4A8u) continue;
            auto* ts = th->thread_state();
            if (!ts) continue;
            auto* mc = ts->context();
            if (!mc) continue;
            uint32_t seen[96];
            int ns = 0;
            for (int it = 0; it < 8000 && ns < 94; it++) {
              uint32_t pc = static_cast<uint32_t>(mc->lr);
              if (pc >= 0x82000000u && pc < 0x84000000u) {
                bool dup = false;
                for (int j = 0; j < ns; j++)
                  if (seen[j] == pc) { dup = true; break; }
                if (!dup) seen[ns++] = pc;
              }
              std::this_thread::yield();
            }
            char mb[760];
            int mo = 0;
            mb[0] = 0;
            for (int j = 0; j < ns && mo < 720; j++) {
              int n = std::snprintf(mb + mo, sizeof(mb) - mo, "%x ", seen[j]);
              if (n > 0) mo += n;
            }
            REXKRNL_INFO("GEWATCHDOG MAINPATH (unique lr x{}): {}", ns, mb);
            // Snapshot the main thread's full stack repeatedly with SLEEPS (no
            // spinning -> doesn't starve it, it keeps cycling). Log only snapshots
            // where it is OUTSIDE the frame-limiter -> in the per-frame render
            // path -> the render call chain + the skipped 3D-submit branch.
            {
              int logged = 0;
              for (int snap = 0; snap < 160 && logged < 12; snap++) {
                uint32_t pc = static_cast<uint32_t>(mc->lr);
                bool in_lim = (pc >= 0x823B3040u && pc <= 0x823B3540u) ||
                              (pc >= 0x82189DC0u && pc <= 0x82189E14u);
                if (!in_lim && pc >= 0x82000000u && pc < 0x84000000u) {
                  uint32_t sp = mc->r1.u32;
                  char fb[620];
                  int fo = std::snprintf(fb, sizeof(fb), "lr=%x | ", pc);
                  if (sp >= 0x10000u && sp < 0xC0000000u) {
                    uint8_t* hsp = base + sp;
                    const uint8_t* send = ge_readable_end(hsp, 0x2800u);
                    if (send > hsp) {
                      for (uint8_t* pp = hsp; pp + 4 <= send && fo < 580; pp += 4) {
                        uint32_t v;
                        std::memcpy(&v, pp, 4);
                        v = __builtin_bswap32(v);
                        if (v >= 0x82000000u && v < 0x84000000u) {
                          int n = std::snprintf(fb + fo, sizeof(fb) - fo, "%x ", v);
                          if (n > 0) fo += n;
                        }
                      }
                    }
                  }
                  REXKRNL_INFO("GEWATCHDOG FRAMEWORK[{}] {}", logged, fb);
                  logged++;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
              }
            }
            break;
          }
        }
      }
    } else {
      stall = 0;
      logged = false;
      recover_fired = false;
    }
    last_wpi = wpi; last_rpi = rpi; last_present = present; last_submit = submit;
  }
}

inline void ge_start_watchdog_once() {
  // Detection + logging run by DEFAULT (a 250ms-poll detached thread, negligible
  // cost) so a rare in-the-wild lock-up is captured in ge.log without needing
  // ge_freeze_diag set ahead of time. The active memory-write auto-recovery
  // inside the loop stays gated behind ge_freeze_diag, so the default behaviour is
  // purely observational.
  static std::atomic<bool> started{false};
  bool expected = false;
  if (started.compare_exchange_strong(expected, true)) {
    std::thread(ge_watchdog_thread).detach();
  }
}
}  // namespace

// NOTE: no frame-limiter / intro-wait hook. The post-intro freeze is a
// SYMPTOM of the rexglue GPU command-processor not consuming the ring (GPU
// hung -> game stops presenting -> guest time stops -> wait never clears).
// Any hook writing that shared time counter corrupts per-frame timing and
// slows the intros without fixing the GPU hang -> net-harmful, removed.
// Intros run at full speed; post-intro hits the rexglue GPU ceiling.

// sub_82198C28 frame-wait reads now = *(r9+0x58) (r9 = *(r13+0x100)), waits
// while (now - last) < 0x1388. That field is a hardware/kernel time the game
// only READS (no guest writer); rexglue never ticks it -> frozen at 0 ->
// infinite spin. Feed it the real guest tick clock (REX_QUERY_TIMEBASE, the
// same ~49.875MHz source mftb uses): write the live value to both the loaded
// register (so this iteration's compare sees it) and the memory field (so
// other readers/sub_8235EAA8 see a consistent advancing clock). (now-last)
// then measures real elapsed ticks exactly like console -> correct pacing.
void ge_dbg_now(PPCRegister& r9, PPCRegister& r30) {
  PPCContext* ctx; uint8_t* base; getcb(ctx, base);
  uint32_t t = (uint32_t)REX_QUERY_TIMEBASE();
  if (r9.u32) ST32(base, r9.u32 + 0x58, t);
  r30.u32 = t;

  uint32_t dev = ctx->r29.u32;
  uint32_t idblk = ctx->r11.u32;
  uint32_t ws = ctx->r31.u32;

  // Feed the freeze watchdog (stash device pointers, count polls, start thread).
  g_ge_device.store(dev, std::memory_order_relaxed);
  g_ge_idblk.store(idblk, std::memory_order_relaxed);
  g_dbgnow_calls.fetch_add(1, std::memory_order_relaxed);
  ge_start_watchdog_once();
  auto* cpp = ge_cp();
  uint32_t cpc = cpp ? cpp->counter() : 0;
  uint32_t rpi = cpp ? static_cast<CPProbe*>(cpp)->rpi() : 0;
  uint32_t wpi = cpp ? static_cast<CPProbe*>(cpp)->wpi() : 0;

  if (ws) ST32(base, ws + 12, t);

  // Clear the GPU-completion fence once the just-submitted frame is drawn.
  // Two race-free conditions:
  //  (a) CP swap counter advanced past the value sampled at the matching
  //      present -> the frame's swap executed; OR
  //  (b) the CP read pointer caught up to the write pointer -> the CP consumed
  //      every packet the game submitted (this frame's draws+resolve+swap), so
  //      it is drawn. (b) eliminates the cpc-sampling race (CP finishing before
  //      ge_diag_vdswap samples g_present_cpcnt) that intermittently left the
  //      wait spinning forever -> the random menu freeze. The CP advances rptr
  //      on its own worker thread and always catches up once the game stops
  //      feeding the ring, so (b) cannot deadlock.
  bool drawn = (cpc != g_present_cpcnt.load(std::memory_order_relaxed)) ||
               (wpi != 0u && rpi == wpi);

  //  (c) WATCHDOG. If neither (a) nor (b) has happened for a long stretch
  //      (~80ms of real wall time), the CP is genuinely stuck on this frame
  //      (e.g. blocked in a D3D12 op the backend can't complete for this
  //      title's scene). Force-complete so the guest's GPU-completion spin
  //      clears instead of deadlocking forever -- and, critically, releases the
  //      device spinlock other guest threads (sub_821A3A40) are blocked on.
  //      This turns a permanent freeze into a recoverable hitch. 80ms is far
  //      above any real frame time, so normal frames still clear via (a)/(b).
  // sub_82198C28 checks the skip bit *(device+10941)&2 at its very top and
  // returns 0 (proceed) before any fence/timeout logic -- the only GUARANTEED
  // way out of the spin. Writing the completion fence does not always release
  // it (it resets the routine's own timeout anchor, and only helps when
  // submit>=target). So: when the wait has stalled for a real wall-clock
  // stretch (~80ms, far above any frame), SET the skip bit so the guest stops
  // blocking on a GPU completion the CP cannot deliver -- and, crucially,
  // releases the device spinlock the rest of the guest threads are stuck on.
  // When the CP is keeping up (drawn via (a)/(b)) CLEAR it again so waits are
  // honored and frames stay visible. Net: visible when the GPU keeps up, a
  // brief skipped (black) frame during a stall instead of a permanent freeze.
  // Do NOT touch the skip bit on the normal/keeping-up path -- the game manages
  // *(device+10941)&2 itself (sets it when it intends NOT to block, clears it
  // when it wants to wait), and clearing it during early init hangs the boot.
  // Only SET it when the wait has genuinely stalled (~80ms, far above any real
  // frame): that forces sub_82198C28 to return 0 next iteration so the guest
  // stops blocking on a GPU completion the CP cannot deliver -- breaking the
  // spinlock cascade / freeze. The game re-clears it on its own next frame, so
  // this stays a one-shot "proceed past this stall", not a permanent skip.
  static thread_local uint32_t s_wait_start = 0;
  static thread_local bool s_waiting = false;
  static thread_local bool s_fallback_counted = false;
  if (!drawn) {
    if (!s_waiting) { s_waiting = true; s_wait_start = t; s_fallback_counted = false; }
    else if ((uint32_t)(t - s_wait_start) > 4000000u) {  // ~80ms @49.875MHz
      drawn = true;
      if (dev) base[dev + 10941u] |= 0x02u;   // stalled: skip this GPU wait
      // Count once per stall episode (this branch re-runs every poll while the
      // stall persists); the watchdog reports it as a residual-mechanism signal.
      if (!s_fallback_counted) {
        s_fallback_counted = true;
        g_skipbit_fallback_fires.fetch_add(1, std::memory_order_relaxed);
      }
    }
    // The six GPU-completion waits poll this routine. Yield the core back so the
    // rexglue CP worker (the thread that advances the ring read pointer / swap
    // counter to satisfy (a)/(b)) can run; we re-check (a)/(b) on the next poll.
    //
    // This is now unified across arches. It used to be arch-gated: arm64 blocked
    // on rex_ge_cp_wait_progress because a tight yield busy-spin from dozens of
    // guest threads oversubscribed the few handheld cores and starved the CP
    // worker (the fence never advanced -> freeze). But the blocking wait
    // re-checks only every ~2ms, so the ~80ms skip-bit fallback above latches and the game
    // wedges (render skipped every frame) -- the SAME freeze the desktop bisect
    // (commit 9dd0258) pinned on the blocking wait. The real fix is to stop the
    // starvation at its source: the CP worker is now created at kAboveNormal
    // priority (command_processor.cpp), so it stays scheduled even while guest
    // threads yield-spin -- making the proven-good desktop yield path correct on
    // arm64 too. Keep the yield; the rexglue handshake API stays defined as a
    // cheap escape hatch if the priority boost ever proves insufficient.
    if (!drawn) {
      std::this_thread::yield();
    }
  } else {
    if (s_waiting) {
      // Wait episode over: bill the yield-spin to the per-frame stage
      // counters (guest timebase ticks -> us; /50 approximates the 49.875MHz
      // timebase within 0.25%). Summed across ALL waiting guest threads, so
      // it reads as total thread-time blocked on the GPU, not wall time.
      rex::perf::IncrementCounter(rex::perf::CounterId::kGuestGpuWaitUs,
                                  static_cast<int64_t>((uint32_t)(t - s_wait_start) / 50u));
    }
    s_waiting = false;
  }

  if (dev && idblk && idblk < 0xFFFFFFFEu && drawn) {
    // DO NOT write idblk+0 here. idblk+0 is the CPU<->GPU semaphore the CP polls
    // in WAIT_REG_MEM (waits for ==0; the render writes 0 to release the CP).
    // Writing a non-zero "completed" value here HELD the semaphore -> the CP
    // stalled in WAIT_REG_MEM -> CPU<->GPU deadlock -> the visual freeze. THIS
    // self-inflicted write was the freeze. (Confirmed via the WAIT_REG_MEM
    // >60ms deadlock-breaker log polling exactly this address.)
    ST32(base, dev + 16552, LD32(base, dev + 16544));   // presented := submit
    ST32(base, idblk + 60, rpi);                        // ring RPTR write-back

    // Real-render heartbeat. This branch runs ONLY when the guest actually
    // consumed a GPU completion (drawn) and advanced presented:=submit -- i.e. a
    // frame really reached the screen. Unlike the "GEGPU present#" heartbeat (and
    // the CP swap counter), which keep advancing on a FROZEN boot (VdSwap still
    // fires, render skipped), this one stops the instant the game wedges. The
    // Android boot loader (GoldenEyeActivity.hasStartedPresenting) greps for it so
    // a frozen boot is detected and relaunched instead of mistaken for "live".
    //
    // Count REAL frames, not poll iterations: this branch runs thousands of times
    // per frame (the guest polls the GPU wait in a tight loop), so gate on the
    // submit counter (dev+16544) actually advancing, then log once per 64 frames.
    uint32_t submit = LD32(base, dev + 16544);
    // Dedup the frame transition ATOMICALLY. The six GPU-completion waits poll
    // this branch from multiple guest threads; a plain read-modify-write let two
    // of them observe the same submit advance and both run the body -- a benign
    // dup for the heartbeat, but it double-counted the frame in the FPS recorder
    // (two OnFrame() calls microseconds apart -> bogus sub-ms frames that wrecked
    // the best-frame metric). A CAS makes exactly one caller win each distinct
    // submit value, so OnFrame() fires once per real frame.
    static std::atomic<uint32_t> s_last_submit{0};
    static std::atomic<uint32_t> rendered{0};
    uint32_t prev_submit = s_last_submit.load(std::memory_order_relaxed);
    if (submit != prev_submit &&
        s_last_submit.compare_exchange_strong(prev_submit, submit,
                                              std::memory_order_relaxed)) {
      // How many frames the counter really advanced by: a poll can miss an
      // intermediate submit value (clear+present pairs, catch-up after a
      // hitch), and telling the recorder lets it split the elapsed time into
      // per-frame samples instead of booking one doubled "slow" frame. Wrap-
      // safe u32 subtract; the recorder clamps to [1,4]. prev_submit==0 means
      // first observation -> treat as a single frame.
      uint32_t adv = (prev_submit != 0) ? (submit - prev_submit) : 1u;
      ge::FpsOnFrame(adv);  // feed the FPS benchmark recorder
      uint32_t old = rendered.fetch_add(1, std::memory_order_relaxed);
      if ((old & 0x3F) == 0)  // log #1, #65, #129... (boot loader greps >= 65)
        REXKRNL_INFO("GEGPU rendered#{} dev={:#x} submit={}", old + 1, dev, submit);
    }
  }
}

// ---------------------------------------------------------------------------
// GPU-completion fence (the real fix). Wired at the present path
// sub_821996F8 @ 0x82199948 (right after the kernel VdSwap), r31 = a1 (D3D
// device struct), r30 = v21 (cmd-buffer swap slot).
//
// At each guest present, sample rexglue's CP swap counter. The poll hook
// (ge_dbg_now) then treats the frame as GPU-complete only once the CP's
// counter has moved past this -- i.e. the just-submitted frame was really
// drawn -- so the game blocks for the real render (visible) but no longer.
void ge_diag_vdswap(PPCRegister& r31, PPCRegister& r30) {
  PPCContext* ctx; uint8_t* base; getcb(ctx, base);
  (void)r30;
  uint32_t a1 = r31.u32;
  auto* cpp = ge_cp();
  uint32_t cpc = cpp ? cpp->counter() : 0;
  g_present_cpcnt.store(cpc, std::memory_order_relaxed);
  g_present_tb.store((uint32_t)REX_QUERY_TIMEBASE(), std::memory_order_relaxed);

  // Throttled present heartbeat. This is ALSO the boot-success signal the Android
  // loader (GoldenEyeActivity.hasStartedPresenting) greps ge.log for ("present#N",
  // N >= PRESENT_THRESHOLD); it must NOT be gated behind ge_freeze_diag (default
  // off) or the loader never detects a live render loop and retries/spins forever
  // even though the guest is presenting. One line per ~64 presents is negligible.
  static uint32_t n = 0;
  if ((n++ & 0x3F) == 0)
    REXKRNL_INFO("GEGPU present#{} dev={:#x} cpcnt={}", n, a1, cpc);

  // Pump the game-state bridge once per present (== once per displayed frame):
  // snapshot carried weapons / ammo / equipped id for the second-screen weapon
  // menu, and apply any pending equip request. Inert until the guest inventory
  // layout is confirmed on-device (see ge_gamestate.cpp); safe to call here.
  ge::gamestate::OnFrame(ctx, base);

  // Pace the second-screen weapon menu: request one UI-thread paint of the
  // secondary surface for this frame. Runs on the game thread, so it only posts
  // a deferred UI-thread tick (the secondary surface must be touched only there)
  // -- and it is a cheap no-op when there is no second screen / the feature is
  // off, which is the single-screen fallback. See ge_dualscreen.cpp.
  ge::DualScreen::Get().OnGuestPresent();
}

// F3  0x830E0670 (site 0x8209F5F0 sub_8209F5D8 -> ge_cont_8209F5F4)
void ge_hook_830E0670(PPCRegister& r3, PPCRegister& r11, PPCRegister& r28) {
  PPCContext* ctx; uint8_t* base; getcb(ctx, base);
  r11.u32 = r3.u32 ^ 0x2Bu;
  r28.u32 = 0x82420000u;
  base[0x82420239u] = static_cast<uint8_t>(r11.u32 & 0xFFu);
  r28.u32 = 0x82420239u;
  r11.u32 = 0x82000000u;
  ge_cont_8209F5F4(*ctx, base);
}

// F1  0x830E0630: the r30++ (with 3/6 skip) loop-increment fragment. Hooked at
// the branch site 0x820F774C; the config jump_address sends control back to
// 0x820F7750 (cmpwi r30,8 / blt loc_820F768C) IN THE PARENT sub_820F73F8, so the
// whole loop -- including the loop-back to 0x820F768C -- stays in one function
// and resolves. (Routing through a separate ge_cont_820F7750 left that loop-back
// branch cross-function -> REX_FATAL when the loop ran, e.g. at the main menu.)
bool ge_hook_830E0630(PPCRegister& r30) {
  r30.u32 = r30.u32 + 1;
  if (r30.s32 == 3 || r30.s32 == 6) r30.u32 = r30.u32 + 1;
  // 0x820F7750: cmpwi r30,8 ; 0x820F7754: blt loc_820F768C (loop) else exit.
  return r30.s32 < 8;
}

// F2  sub_820F7968: r26 0..8 loop. sub_820F7968 = prologue only (codegen sets
// constants + r26=0). ge_f2_driver fires after the last prologue instruction
// (0x820F79EC, after, return) and drives the loop:
//   do { body; r26++; if(r26==3||r26==6) r26++; } while (r26 < 8); epilogue
// ge_body_820F79F0 = 0x820F79F0..0x820F7CFC (one iteration). Its skip branch
// (0x820F7A2C: clrlwi r11,r3,24; cmplwi r11,0; beq loc_820F7D00) becomes a
// return from the body via ge_f2_skip (return_on_true).
bool ge_f2_skip(PPCRegister& r3) {
  return (r3.u32 & 0xFFu) == 0u;  // beq loc_820F7D00 taken when (r3&0xFF)==0
}
void ge_f2_driver(PPCRegister& r26) {
  PPCContext* ctx; uint8_t* base; getcb(ctx, base);
  for (;;) {
    ge_body_820F79F0(*ctx, base);                 // 0x820F79F0 one iteration
    r26.u32 = r26.u32 + 1;                         // 0x830E06B0 increment
    if (r26.s32 == 3 || r26.s32 == 6) r26.u32 = r26.u32 + 1;
    if (r26.s32 >= 8) break;                       // 0x820F7D08 blt not taken
  }
  ge_epi_820F7D0C(*ctx, base);                     // 0x820F7D0C epilogue (ret)
}

// F4  0x830E0200: loop-increment fragment (same shape as F1). Hooked at the
// branch site 0x820C4914; the config jump_address sends control back to
// 0x820C4918 IN THE PARENT sub_820C4630 so the loop-back to 0x820C4858 resolves
// in-function instead of crossing into a ge_cont_820C4918 (-> REX_FATAL).
bool ge_hook_830E0200(PPCRegister& r31, PPCRegister& r29, PPCRegister& r28,
                      PPCRegister& r11, PPCRegister& r23, PPCRegister& r21) {
  PPCContext* ctx; uint8_t* base; getcb(ctx, base);
  if (r31.s32 == 3) { r31.u32 = r31.u32 + 1; r29.u32 = r29.u32 + 4; }
  r28.u32 = r11.u32 + r28.u32;
  // 0x820C4918: cmpw r31,r23 ; 0x820C491C: ble loc_820C4858 (loop) else exit.
  // The loop top 0x820C4858 (lwz r11,-0x684(r21)) is skipped on first entry
  // (0x820C4854 b loc_820C485C) -> only the loop-back reaches it, so it is not a
  // standalone block. Do its r11 reload here and jump to 0x820C485C (which IS
  // reachable / labeled) instead.
  if (r31.s32 <= r23.s32) {
    r11.u32 = LD32(base, r21.u32 - 0x684u);   // 0x820C4858: lwz r11,-0x684(r21)
    return true;                              // -> loc_820C485C (loop body)
  }
  return false;                               // -> loc_820C4920 (exit)
}

// F5  0x830E04D0 (site 0x820C7450 sub_820C7390 -> ge_cont_820C7454)
void ge_hook_830E04D0(PPCRegister& r11, PPCRegister& r10, PPCRegister& r9) {
  PPCContext* ctx; uint8_t* base; getcb(ctx, base);
  if (r11.s32 == 3) r11.u32 = r11.u32 - 1;
  ST32(base, r9.u32 - 0x644u, r10.u32);
  ge_cont_820C7454(*ctx, base);
}

// F6  0x830E0560 (site 0x820C742C sub_820C7390 -> ge_cont_820C7430)
void ge_hook_830E0560(PPCRegister& r11, PPCRegister& r10, PPCRegister& r9) {
  PPCContext* ctx; uint8_t* base; getcb(ctx, base);
  if (r11.s32 == 3) r11.u32 = r11.u32 + 1;
  ST32(base, r9.u32 - 0x644u, r10.u32);
  ge_cont_820C7430(*ctx, base);
}

// F7  0x830E0460 (site 0x820A3E50 sub_820A3C20 -> ge_cont_820A3E9C)
void ge_hook_830E0460(PPCRegister& r11, PPCRegister& r4, PPCRegister& r29,
                      PPCRegister& r7, PPCRegister& r28, PPCRegister& r6,
                      PPCRegister& r31, PPCRegister& r5, PPCRegister& r27,
                      PPCRegister& r3) {
  PPCContext* ctx; uint8_t* base; getcb(ctx, base);
  r4.s64 = static_cast<int64_t>(static_cast<int16_t>(r11.u32 & 0xFFFFu));
  r7.u64 = r29.u64;
  r6.u32 = LD32(base, r28.u32 + 0x4DE8u);
  r5.u64 = r31.u64;
  r3.u32 = LD32(base, r27.u32 + 0x4DE0u);
  sub_82144920(*ctx, base);
  r3.u32 = 0;
  ge_cont_820A3E9C(*ctx, base);
}

// F8  0x830E0750 (site 0x820B40E4 sub_820B40C0, returns; code ends in blr)
void ge_hook_830E0750(PPCRegister& r7, PPCRegister& r8, PPCRegister& r11,
                      PPCRegister& f1) {
  PPCContext* ctx; uint8_t* base; getcb(ctx, base); (void)ctx;
  uint32_t v32 = LD32(base, r8.u32 - 0x564u);
  r7.u32 = v32;
  if (v32 != 0) return;
  uint64_t v64 = LD64(base, r8.u32 - 0x560u);
  r7.u64 = v64;
  if (v64 != 0) return;
  STF32(base, r11.u32 + 0x1F0u, f1.f32);
  uint32_t t = LD32(base, r11.u32 + 0x1E4u);
  r8.u32 = t;
  ST32(base, r11.u32 + 0x1ECu, t);
  r8.u32 = 0;
  ST32(base, r11.u32 + 0x200u, 0);
}

// ===========================================================================
// Mouse-look (real 1:1 keyboard/mouse looking, replacing the stick-emulation
// path). We inject raw mouse deltas straight into the guest's per-frame look
// state inside ge_bondview_control (sub_820B99E8), so the mouse drives the same
// heading/pitch the right stick does -- but without the analog deadzone/accel/
// turn-rate cap that makes the built-in MnK stick emulation feel terrible.
//
// The look injection is platform-neutral (pure guest-memory writes). The input
// SOURCE (relative mouse delta, key-down state, cursor capture) is supplied by
// GameInputListener below, a cross-platform rex::ui::WindowInputListener built
// on the Window abstraction (Win32 + GTK), replacing upstream's Win32-only raw
// input thread.
//
// Mouse and controller both work AT ONCE (additive). While mouse-look is on we
// capture the OS cursor (hidden + centered) during play, and release it whenever
// the pause menu is open or the window loses focus.
// ===========================================================================

// Mouse-look tunables, ported from the xenia-canary mousehook cvars. The
// user-facing sensitivity multiplier is ge_mouse_sens (defined below).
REXCVAR_DEFINE_BOOL(ge_invert_x, false, "Input", "Invert mouse X (horizontal) look");
REXCVAR_DEFINE_BOOL(ge_invert_y, false, "Input", "Invert mouse Y (vertical) look");
REXCVAR_DEFINE_BOOL(ge_disable_autoaim, true, "Input",
                    "Disable auto-aim and look-ahead while mouse-look is on");
REXCVAR_DEFINE_DOUBLE(ge_menu_sensitivity, 1.0, "Input",
                      "Mouse sensitivity in menus").range(0.05, 20.0);
REXCVAR_DEFINE_DOUBLE(ge_aim_turn_distance, 0.4, "Input",
                      "Crosshair travel in aim-mode before the camera turns [0-1]").range(0.0, 1.0);
REXCVAR_DEFINE_BOOL(ge_gun_sway, true, "Input", "Gun sway as the camera turns");

REXCVAR_DEFINE_DOUBLE(ge_mouse_sens, 1.0, "Input", "Mouse look sensitivity").range(0.05, 20.0);
REXCVAR_DEFINE_DOUBLE(ge_mouse_smooth, 0.0, "Input",
                      "Mouse look smoothing, EMA over per-frame deltas (0 = off/raw .. 0.9 = heavy)")
    .range(0.0, 0.9);
// Mouse-look on/off. ON: the mouse looks (added on top of the pad -- both work
// at once, so you can put the controller down) and the cursor is captured during
// play. OFF: no mouse-look, cursor free, controller only.
REXCVAR_DEFINE_BOOL(ge_mouselook_enable, true, "Input",
                    "Mouse look (works alongside the controller; captures the cursor in-game)");

namespace {
bool ge_mouselook_on() { return REXCVAR_GET(ge_mouselook_enable); }

// Cross-platform mouse/keyboard source. Registered on the rexglue display
// window, it accumulates relative mouse delta + a key-down table (read by the
// guest mid-asm hooks) and owns the cursor-capture lifecycle through the Window
// abstraction. Modeled on rexglue's MnkInputDriver, which uses the identical
// Win32+GTK capture/warp/delta pattern. Listener callbacks fire on the UI
// thread; the guest hooks read on guest threads, so shared state is guarded.
class GameInputListener final : public rex::ui::WindowInputListener,
                                public rex::ui::WindowListener {
 public:
  void Attach(rex::ui::Window* w) {
    window_ = w;
    if (window_) {
      window_->AddInputListener(this, /*z_order=*/0);
      window_->AddListener(this);
    }
  }

  // Per-frame consumption by the look hook (reset-on-read). Float: raw XI2
  // deltas are sub-pixel, and slow precise aim lives in the fractions.
  float take_dx() { std::lock_guard<std::mutex> l(m_); float v = dx_; dx_ = 0.f; return v; }
  float take_dy() { std::lock_guard<std::mutex> l(m_); float v = dy_; dy_ = 0.f; return v; }

  bool key_down(rex::ui::VirtualKey vk) const {
    uint16_t idx = static_cast<uint16_t>(vk);
    if (idx >= 256) return false;
    std::lock_guard<std::mutex> l(m_);
    return key_down_[idx];
  }

  // Decay the wheel pulse armed by OnMouseWheel into key_down_, once per game
  // frame (called from ge_inject_keyboard, not from key_down() itself, which is
  // polled multiple times per frame and would double-decrement the pulse).
  void tick_wheel() {
    std::lock_guard<std::mutex> l(m_);
    key_down_[static_cast<uint16_t>(rex::ui::VirtualKey::kMouseWheelUp)] = wheel_up_frames_ > 0;
    key_down_[static_cast<uint16_t>(rex::ui::VirtualKey::kMouseWheelDown)] = wheel_down_frames_ > 0;
    if (wheel_up_frames_ > 0) --wheel_up_frames_;
    if (wheel_down_frames_ > 0) --wheel_down_frames_;
  }

  bool focused() const { return window_ && window_->HasFocus(); }
  bool suppressed() const { return suppressed_.load(std::memory_order_relaxed); }

  void set_suppressed(bool v) {
    suppressed_.store(v, std::memory_order_relaxed);
    if (v) { std::lock_guard<std::mutex> l(m_); dx_ = 0.f; dy_ = 0.f; }  // drop queued motion
    tick_capture();  // release the cursor immediately when the menu opens
  }

  // Engage/release capture + recenter. Independent of the FP-mode gate (matches
  // upstream): keeps the cursor hidden + pinned whenever look is on and focused.
  void tick_capture() {
    if (!window_) return;
    std::lock_guard<std::mutex> cl(cap_m_);
    const bool want = ge_mouselook_on() && !suppressed_.load(std::memory_order_relaxed) &&
                      window_->HasFocus();
    if (want && !captured_) {
      captured_ = true;
      window_->SetCursorVisibility(rex::ui::Window::CursorVisibility::kHidden);
      window_->CaptureMouse();
      std::lock_guard<std::mutex> l(m_);  // no spike on capture start
      dx_ = 0.f; dy_ = 0.f; have_prev_ = false;
      raw_motion_ = false;  // re-arm the warp fallback until raw deltas flow again
    } else if (!want && captured_) {
      captured_ = false;
      window_->SetCursorVisibility(rex::ui::Window::CursorVisibility::kVisible);
      window_->ReleaseMouse();
    }
    if (captured_) {
      // Re-center each tick (prevents edge clamping). Seed prev to the center
      // first so the warp's echoed OnMouseMove yields a zero delta.
      const int cx = static_cast<int>(window_->GetActualLogicalWidth() / 2);
      const int cy = static_cast<int>(window_->GetActualLogicalHeight() / 2);
      { std::lock_guard<std::mutex> l(m_); prev_x_ = cx; prev_y_ = cy; have_prev_ = true; }
      window_->WarpMouseToClient(cx, cy);
    }
  }

  // WindowInputListener
  void OnMouseMove(rex::ui::MouseEvent& e) override {
    std::lock_guard<std::mutex> l(m_);
    if (raw_motion_) return;  // raw XI2 deltas drive look; warped positions are noise
    const int x = e.x(), y = e.y();
    if (have_prev_) { dx_ += float(x - prev_x_); dy_ += float(y - prev_y_); }
    prev_x_ = x; prev_y_ = y; have_prev_ = true;
  }
  // Raw unaccelerated deltas (X11 XI2 while captured). Once these flow, the
  // position-difference path above is skipped: it double-counts the same
  // motion and re-adds the OS acceleration curve the raw path exists to avoid.
  void OnMouseRelativeMotion(rex::ui::MouseRelativeEvent& e) override {
    std::lock_guard<std::mutex> l(m_);
    raw_motion_ = true;
    dx_ += e.dx(); dy_ += e.dy();
  }
  void OnMouseDown(rex::ui::MouseEvent& e) override { set_mouse_button(e.button(), true); }
  void OnMouseUp(rex::ui::MouseEvent& e) override { set_mouse_button(e.button(), false); }
  void OnKeyDown(rex::ui::KeyEvent& e) override { set_key(e.virtual_key(), true); }
  void OnKeyUp(rex::ui::KeyEvent& e) override { set_key(e.virtual_key(), false); }
  // Wheel notches are edge-only (one callback per detent, no held state at the
  // window layer -- see rex::ui::WindowInputListener::OnMouseWheel). Arm a short
  // pulse here; tick_wheel() decays it into key_down_ once per game frame, the
  // same pattern rexglue's MnkInputDriver uses (mnk_wheel_pulse_frames, default
  // 2) for its own polled key table.
  void OnMouseWheel(rex::ui::MouseEvent& e) override {
    if (REXCVAR_GET(ge_gamestate_diag))
      REXKRNL_INFO("GEWHEEL event dy={}", e.scroll_y());
    std::lock_guard<std::mutex> l(m_);
    constexpr int kPulseFrames = 2;  // matches mnk_wheel_pulse_frames default
    if (e.scroll_y() > 0) wheel_up_frames_ = kPulseFrames;
    else if (e.scroll_y() < 0) wheel_down_frames_ = kPulseFrames;
    if (REXCVAR_GET(ge_gamestate_diag))
      REXKRNL_INFO("GEWHEEL armed up={} down={}", wheel_up_frames_, wheel_down_frames_);
  }

  // WindowListener: drop held keys / queued motion on focus loss (no stuck keys
  // or view snap on alt-tab). Capture itself auto-releases via tick_capture.
  void OnLostFocus(rex::ui::UISetupEvent&) override {
    std::lock_guard<std::mutex> l(m_);
    std::memset(key_down_, 0, sizeof(key_down_));
    dx_ = 0.f; dy_ = 0.f; have_prev_ = false; raw_motion_ = false;
    wheel_up_frames_ = 0; wheel_down_frames_ = 0;
  }

 private:
  void set_key(rex::ui::VirtualKey vk, bool down) {
    uint16_t idx = static_cast<uint16_t>(vk);
    if (idx >= 256) return;
    std::lock_guard<std::mutex> l(m_);
    key_down_[idx] = down;
  }
  void set_mouse_button(rex::ui::MouseEvent::Button b, bool down) {
    using B = rex::ui::MouseEvent::Button;
    rex::ui::VirtualKey vk = rex::ui::VirtualKey::kNone;
    switch (b) {
      case B::kLeft: vk = rex::ui::VirtualKey::kLButton; break;
      case B::kRight: vk = rex::ui::VirtualKey::kRButton; break;
      case B::kMiddle: vk = rex::ui::VirtualKey::kMButton; break;
      case B::kX1: vk = rex::ui::VirtualKey::kXButton1; break;
      case B::kX2: vk = rex::ui::VirtualKey::kXButton2; break;
      default: return;
    }
    set_key(vk, down);
  }

  mutable std::mutex m_;     // guards dx_/dy_/prev_*/have_prev_/raw_motion_/key_down_
  std::mutex cap_m_;         // serializes capture transitions (captured_)
  rex::ui::Window* window_ = nullptr;
  float dx_ = 0.f, dy_ = 0.f;
  int prev_x_ = 0, prev_y_ = 0;
  bool have_prev_ = false;
  bool raw_motion_ = false;  // XI2 raw deltas flowing -> position diffs are noise
  bool captured_ = false;
  std::atomic<bool> suppressed_{false};  // true while the pause menu is open
  bool key_down_[256] = {};
  int wheel_up_frames_ = 0, wheel_down_frames_ = 0;  // armed by OnMouseWheel, decayed by tick_wheel()
};

GameInputListener g_listener;
std::atomic<bool> g_listener_attached{false};

// Attach the listener to the display window the first time it exists (it may be
// null at OnCreateDialogs time). Called from InitMouseLook and the per-frame
// hooks, so attachment is robust regardless of startup ordering.
void ge_ensure_listener() {
  if (g_listener_attached.load(std::memory_order_relaxed)) return;
  rex::Runtime* rt = rex::Runtime::instance();
  rex::ui::Window* w = rt ? rt->display_window() : nullptr;
  if (!w) return;
  bool expected = false;
  if (g_listener_attached.compare_exchange_strong(expected, true)) {
    g_listener.Attach(w);
    REXKRNL_INFO("GEMOUSE listener attached window={}", (void*)w);
  }
}

float ge_take_mouse_dx() { return g_listener.take_dx(); }
float ge_take_mouse_dy() { return g_listener.take_dy(); }
}  // namespace

namespace ge {
// Attach the cross-platform mouse/keyboard listener at app startup (or lazily on
// the first guest frame if the window isn't ready yet).
void InitMouseLook() {
  REXKRNL_INFO("GEMOUSE InitMouseLook (enable={})", REXCVAR_GET(ge_mouselook_enable));
  ge_ensure_listener();
}

// Called by the app when the pause menu opens/closes so mouse motion isn't
// turned into look while the player is in menus (the cursor is needed there).
void SetMouselookSuppressed(bool v) { g_listener.set_suppressed(v); }
}  // namespace ge

// ===========================================================================
// Mouse-look: faithful port of the xenia-canary mousehook
// (src/xenia/hid/winkey/hookables/goldeneye.cc, GoldeneyeGame::DoHooks) for the
// GoldenEye_Nov2007_Release build. Runs once per frame from ge_inject_keyboard,
// operating on the local player struct in guest RAM. Writes the game's own
// camera / crosshair / gun fields incrementally so it coexists with the
// controller, recoil, and the tank turret.
// ===========================================================================
namespace {
// RareGameBuildAddrs for GoldenEye_Nov2007_Release (from supported_builds[]).
constexpr uint32_t GE_MENU_XY       = 0x8272B37Cu;  // menu cursor X (Y at +4)
constexpr uint32_t GE_PAUSE_FLAG    = 0x82F1E70Cu;  // non-zero ~= game paused
constexpr uint32_t GE_SETTINGS_PTR  = 0x83088228u;  // -> settings struct pointer
constexpr uint32_t GE_SETTINGS_BITS = 0x298u;       // bitflags offset in struct
constexpr uint32_t GE_PLAYER_PTR    = 0x82F1FA98u;  // -> players[0] (host's Bond)
constexpr uint32_t GE_BONDVIEW_CUR  = 0x82F1FAACu;  // -> currently-controlled view's player
constexpr uint32_t GE_OFF_WATCH     = 0x2E8u;       // watch status (!=0 -> input disabled)
constexpr uint32_t GE_OFF_DISABLED  = 0x80u;        // control-disabled flag (cutscene)
constexpr uint32_t GE_OFF_CAM_X     = 0x254u;       // camera yaw
constexpr uint32_t GE_OFF_CAM_Y     = 0x264u;       // camera pitch
constexpr uint32_t GE_OFF_CH_X      = 0x10A8u;      // crosshair X
constexpr uint32_t GE_OFF_CH_Y      = 0x10ACu;      // crosshair Y
constexpr uint32_t GE_OFF_GUN_X     = 0x10BCu;      // gun X
constexpr uint32_t GE_OFF_GUN_Y     = 0x10C0u;      // gun Y
constexpr uint32_t GE_OFF_AIM_MODE  = 0x22Cu;       // aim-mode (1 = aiming)
constexpr uint32_t GE_OFF_AIM_MULT  = 0x11ACu;      // aim-turn multiplier (slows when zoomed)
enum GESettingFlag {
  GE_SET_AutoAim   = 0x10,
  GE_SET_LookAhead = 0x80,
};
}  // namespace

void ge_mouse_camera(uint8_t* base) {
  // Persistent state (= GoldeneyeGame member vars in xenia).
  static uint32_t prev_pause = 0, prev_disabled = 0, prev_aim_mode = 0;
  static bool start_centering = false, disable_sway = false;
  static float centering_speed = 0.0125f;

  const float sensitivity = static_cast<float>(REXCVAR_GET(ge_mouse_sens));
  const float menu_sensitivity = static_cast<float>(REXCVAR_GET(ge_menu_sensitivity));
  const bool invert_x = REXCVAR_GET(ge_invert_x);
  const bool invert_y = REXCVAR_GET(ge_invert_y);
  const bool disable_autoaim = REXCVAR_GET(ge_disable_autoaim);
  const float aim_turn_distance = static_cast<float>(REXCVAR_GET(ge_aim_turn_distance));
  const bool gun_sway = REXCVAR_GET(ge_gun_sway);

  // Consume this frame's raw mouse delta once; used for both menu and camera.
  const float mdx = ge_take_mouse_dx();
  const float mdy = ge_take_mouse_dy();

  // Optional look smoothing: EMA over the per-frame delta so frame-pacing
  // jitter doesn't turn 1:1 into uneven yaw steps. Camera/crosshair only --
  // the menu cursor below stays raw (a laggy cursor feels broken). The tail
  // snaps to zero once imperceptible so the camera fully stops.
  static float smooth_dx = 0.f, smooth_dy = 0.f;
  const float smooth = static_cast<float>(REXCVAR_GET(ge_mouse_smooth));
  float cdx = mdx, cdy = mdy;
  if (smooth > 0.f) {
    smooth_dx = smooth_dx * smooth + mdx * (1.f - smooth);
    smooth_dy = smooth_dy * smooth + mdy * (1.f - smooth);
    if (mdx == 0.f && std::fabs(smooth_dx) < 0.01f) smooth_dx = 0.f;
    if (mdy == 0.f && std::fabs(smooth_dy) < 0.01f) smooth_dy = 0.f;
    cdx = smooth_dx; cdy = smooth_dy;
  } else {
    smooth_dx = smooth_dy = 0.f;  // don't carry a stale tail into a re-enable
  }

  // Move the menu selection crosshair (the game's own menus read these).
  {
    float menuX = LDF32(base, GE_MENU_XY);
    float menuY = LDF32(base, GE_MENU_XY + 4);
    menuX += (mdx / 5.f) * menu_sensitivity;
    menuY += (mdy / 5.f) * menu_sensitivity;
    STF32(base, GE_MENU_XY, menuX);
    STF32(base, GE_MENU_XY + 4, menuY);
  }

  // Target the LOCAL player. Online uses Xbox System Link, where each console
  // controls its OWN Bond at a session-global index (host = players[0], clients =
  // players[1..3]). players[0] (GE_PLAYER_PTR) is therefore only the local player
  // on the host -- using it made mouse-look host-only. GE_BONDVIEW_CUR points at
  // the view the local console is actually driving, so it resolves to this
  // console's player in online play and to players[0] in single-player/the host.
  // Target the LOCAL player = the active-viewport player. player+0x904 (viewport
  // size/offset) is 0 only for the view the local console actually renders -- the
  // same signal GoldenEye's own code uses ("current player is the active
  // viewport", per the CE 3D-SFX hack). This is stable every frame and resolves
  // to players[0] on the host, players[1] on a joiner, etc., automatically.
  // (The old GE_BONDVIEW_CUR target flipped between local/remote each frame
  // because the bondview CONTROL loop cycles it across all players -- that was
  // the online jitter.)
  uint32_t player = 0;
  for (int i = 0; i < 4; ++i) {
    uint32_t p = LD32(base, 0x82F1FA98u + i * 4u);
    if (p && LD32(base, p + 0x904u) == 0u) { player = p; break; }
  }
  if (!player) player = LD32(base, GE_BONDVIEW_CUR);  // fallback (menus/boot)
  if (!player) player = LD32(base, GE_PLAYER_PTR);
  if (!player) return;

  const uint32_t game_pause_flag = LD32(base, GE_PAUSE_FLAG);

  // control-disabled (cutscene); fall back to watch-status (watch up/down).
  uint32_t game_control_disabled = LD32(base, player + GE_OFF_DISABLED);
  if (game_control_disabled == 0)
    game_control_disabled = LD32(base, player + GE_OFF_WATCH);

  // Disable auto-aim & look-ahead, only when the pause/control state changes --
  // xenia's exact behaviour. (Doing it every frame oscillated against the game's
  // per-frame auto-aim in multiplayer and caused the camera jitter.)
  if (game_pause_flag != prev_pause || game_control_disabled != prev_disabled) {
    const uint32_t sp = LD32(base, GE_SETTINGS_PTR);
    if (sp) {
      const uint32_t sva = sp + GE_SETTINGS_BITS;
      uint32_t settings = LD32(base, sva);
      if (settings & GE_SET_LookAhead) settings &= ~(uint32_t)GE_SET_LookAhead;
      if (disable_autoaim && (settings & GE_SET_AutoAim))
        settings &= ~(uint32_t)GE_SET_AutoAim;
      ST32(base, sva, settings);
    }
    prev_pause = game_pause_flag;
    prev_disabled = game_control_disabled;
  }

  if (game_control_disabled) return;

  const uint32_t aim_mode = LD32(base, player + GE_OFF_AIM_MODE);
  if (aim_mode != prev_aim_mode) {
    if (aim_mode != 0) {  // entering aim mode -> reset gun position
      STF32(base, player + GE_OFF_GUN_X, 0.f);
      STF32(base, player + GE_OFF_GUN_Y, 0.f);
    }
    // Always reset crosshair on enter/exit (else non-aim fires toward it).
    STF32(base, player + GE_OFF_CH_X, 0.f);
    STF32(base, player + GE_OFF_CH_Y, 0.f);
    prev_aim_mode = aim_mode;
  }

  const float bounds = 1.f, dividor = 500.f, gun_multiplier = 1.f;
  const float crosshair_multiplier = 1.f, centering_multiplier = 1.f;
  const float aim_turn_dividor = 1.f;

  if (aim_mode == 1) {
    float chX = LDF32(base, player + GE_OFF_CH_X);
    float chY = LDF32(base, player + GE_OFF_CH_Y);
    chX += (invert_x ? -1.f : 1.f) * (cdx / dividor) * sensitivity;
    chY += (invert_y ? -1.f : 1.f) * (cdy / dividor) * sensitivity;

    chX = std::min(chX, bounds); chX = std::max(chX, -bounds);
    chY = std::min(chY, bounds); chY = std::max(chY, -bounds);

    STF32(base, player + GE_OFF_CH_X, chX);
    STF32(base, player + GE_OFF_CH_Y, chY);
    STF32(base, player + GE_OFF_GUN_X, chX * gun_multiplier);
    STF32(base, player + GE_OFF_GUN_Y, chY * gun_multiplier);

    // Turn the camera once the crosshair travels past a threshold.
    float camX = LDF32(base, player + GE_OFF_CAM_X);
    float camY = LDF32(base, player + GE_OFF_CAM_Y);
    const float aim_multiplier = LDF32(base, player + GE_OFF_AIM_MULT);
    const float ch_distance = sqrtf(chX * chX + chY * chY);
    if (ch_distance > aim_turn_distance) {
      camX += (chX / aim_turn_dividor) * aim_multiplier;
      STF32(base, player + GE_OFF_CAM_X, camX);
      camY -= (chY / aim_turn_dividor) * aim_multiplier;
      STF32(base, player + GE_OFF_CAM_Y, camY);
    }

    start_centering = true;
    disable_sway = true;       // skip weapon sway until we've centered
    centering_speed = 0.05f;   // speed up centering when leaving aim-mode
  } else {
    float gX = LDF32(base, player + GE_OFF_GUN_X);
    float gY = LDF32(base, player + GE_OFF_GUN_Y);

    // Gun-centering back to the middle after aim-mode / when idle.
    if (start_centering) {
      if (gX != 0 || gY != 0) {
        if (gX > 0) gX -= std::min(centering_speed * centering_multiplier, gX);
        if (gX < 0) gX += std::min(centering_speed * centering_multiplier, -gX);
        if (gY > 0) gY -= std::min(centering_speed * centering_multiplier, gY);
        if (gY < 0) gY += std::min(centering_speed * centering_multiplier, -gY);
      }
      if (gX == 0 && gY == 0) {
        centering_speed = 0.0125f;
        start_centering = false;
        disable_sway = false;
      }
    }

    if (cdx != 0.f || cdy != 0.f) {
      float camX = LDF32(base, player + GE_OFF_CAM_X);
      float camY = LDF32(base, player + GE_OFF_CAM_Y);

      camX += (invert_x ? -1.f : 1.f) * (cdx / 10.f) * sensitivity;

      // Add 'sway' to the gun as the camera turns.
      const float gun_sway_x = ((cdx / 16000.f) * sensitivity) * bounds;
      const float gun_sway_y = ((cdy / 16000.f) * sensitivity) * bounds;
      float gun_sway_x_changed = gX + gun_sway_x;
      float gun_sway_y_changed = gY + gun_sway_y;

      if (!invert_y) {
        camY -= (cdy / 10.f) * sensitivity;
      } else {
        camY += (cdy / 10.f) * sensitivity;
        gun_sway_y_changed = gY - gun_sway_y;
      }

      STF32(base, player + GE_OFF_CAM_X, camX);
      STF32(base, player + GE_OFF_CAM_Y, camY);

      if (gun_sway && !disable_sway) {
        // Bound the sway to [0.2:-0.2] (only if it would push further OOB).
        if (gun_sway_x_changed > (0.2f * bounds) && gun_sway_x > 0) gun_sway_x_changed = gX;
        if (gun_sway_x_changed < -(0.2f * bounds) && gun_sway_x < 0) gun_sway_x_changed = gX;
        if (gun_sway_y_changed > (0.2f * bounds) && gun_sway_y > 0) gun_sway_y_changed = gY;
        if (gun_sway_y_changed < -(0.2f * bounds) && gun_sway_y < 0) gun_sway_y_changed = gY;
        gX = gun_sway_x_changed;
        gY = gun_sway_y_changed;
      }
    } else {
      if (!start_centering) {
        start_centering = true;
        centering_speed = 0.0125f;
      }
    }

    gX = std::min(gX, bounds); gX = std::max(gX, -bounds);
    gY = std::min(gY, bounds); gY = std::max(gY, -bounds);

    STF32(base, player + GE_OFF_CH_X, gX * crosshair_multiplier);
    STF32(base, player + GE_OFF_CH_Y, gY * crosshair_multiplier);
    STF32(base, player + GE_OFF_GUN_X, gX);
    STF32(base, player + GE_OFF_GUN_Y, gY);
  }
}

// ===========================================================================
// Keyboard buttons -> guest gamepad. The right stick (look) is the mouse; every
// other controller input is mapped to a rebindable keyboard key here, injected
// into the polled gamepad buffer so it works alongside a real pad. We do this
// ourselves (not via the SDK's MnK driver) so it can't fight the mouse capture.
//
// Slot-0 gamepad buffer (filled by XamInputGetState in ge_input_poll_controllers,
// Xbox360 big-endian): +0 buttons(u16), +2 LT, +3 RT, +4 LX(s16), +6 LY(s16).
// ===========================================================================
namespace {
constexpr uint32_t GE_PAD0 = 0x830C8B9Cu;  // unk_830C8B9C, slot-0 gamepad

// XInput button bits (match the masks the guest unpacks).
constexpr uint16_t BTN_DPAD_UP = 0x0001, BTN_DPAD_DOWN = 0x0002, BTN_DPAD_LEFT = 0x0004,
                   BTN_DPAD_RIGHT = 0x0008, BTN_START = 0x0010, BTN_BACK = 0x0020,
                   BTN_LTHUMB = 0x0040, BTN_RTHUMB = 0x0080, BTN_LSHOULDER = 0x0100,
                   BTN_RSHOULDER = 0x0200, BTN_A = 0x1000, BTN_B = 0x2000, BTN_X = 0x4000,
                   BTN_Y = 0x8000;

bool ge_input_active() {  // keyboard counts only when focused + not in the menu
  return !g_listener.suppressed() && g_listener.focused();
}

// Is the key currently bound to cvar `name` held down? Reads the bind by name,
// parses it to a virtual key, and polls the cross-platform key-down table.
bool ge_key_down(const char* name) {
  std::string keyname = rex::cvar::GetFlagByName(name);
  if (keyname.empty()) return false;
  rex::ui::VirtualKey vk = rex::ui::ParseVirtualKey(keyname);
  if (vk == rex::ui::VirtualKey::kNone) return false;
  return g_listener.key_down(vk);
}
}  // namespace

// Keyboard binds. Defaults are a placeholder layout; rebindable in the menu.
REXCVAR_DEFINE_BOOL(ge_keyboard_enable, true, "Input", "Map keyboard keys to controller buttons");
REXCVAR_DEFINE_STRING(ge_key_mv_up, "W", "Input/Keybinds", "Move forward (left stick up)");
REXCVAR_DEFINE_STRING(ge_key_mv_down, "S", "Input/Keybinds", "Move back (left stick down)");
REXCVAR_DEFINE_STRING(ge_key_mv_left, "A", "Input/Keybinds", "Move left (left stick left)");
REXCVAR_DEFINE_STRING(ge_key_mv_right, "D", "Input/Keybinds", "Move right (left stick right)");
REXCVAR_DEFINE_STRING(ge_key_a, "Space", "Input/Keybinds", "A button");
REXCVAR_DEFINE_STRING(ge_key_b, "Control", "Input/Keybinds", "B button");
REXCVAR_DEFINE_STRING(ge_key_x, "R", "Input/Keybinds", "X button");
REXCVAR_DEFINE_STRING(ge_key_y, "E", "Input/Keybinds", "Y button");
REXCVAR_DEFINE_STRING(ge_key_lt, "RMB", "Input/Keybinds", "Left trigger");
REXCVAR_DEFINE_STRING(ge_key_rt, "LMB", "Input/Keybinds", "Right trigger");
REXCVAR_DEFINE_STRING(ge_key_lb, "Q", "Input/Keybinds", "Left shoulder");
REXCVAR_DEFINE_STRING(ge_key_rb, "F", "Input/Keybinds", "Right shoulder");
REXCVAR_DEFINE_STRING(ge_key_l3, "C", "Input/Keybinds", "Left stick press");
REXCVAR_DEFINE_STRING(ge_key_r3, "V", "Input/Keybinds", "Right stick press");
REXCVAR_DEFINE_STRING(ge_key_dup, "Up", "Input/Keybinds", "D-pad up");
REXCVAR_DEFINE_STRING(ge_key_ddown, "Down", "Input/Keybinds", "D-pad down");
REXCVAR_DEFINE_STRING(ge_key_dleft, "Left", "Input/Keybinds", "D-pad left");
REXCVAR_DEFINE_STRING(ge_key_dright, "Right", "Input/Keybinds", "D-pad right");
REXCVAR_DEFINE_STRING(ge_key_start, "Return", "Input/Keybinds", "Start button");
REXCVAR_DEFINE_STRING(ge_key_back, "Tab", "Input/Keybinds", "Back button");
REXCVAR_DEFINE_BOOL(ge_weapon_select_enable, true, "Input",
                    "Number keys 1-9 / scrollwheel select carried weapons");
REXCVAR_DEFINE_BOOL(ge_weapon_direct_switch, true, "Input",
                    "Switch weapons via a direct game call (off = Y-cycle walk)");
REXCVAR_DEFINE_STRING(ge_key_wpn_next, "WheelUp", "Input/Keybinds", "Next carried weapon");
REXCVAR_DEFINE_STRING(ge_key_wpn_prev, "WheelDown", "Input/Keybinds", "Previous carried weapon");

// On-screen touch controls (ordinary Android phones/tablets). The virtual pad is
// drawn + hit-tested by the Java overlay (TouchControlsView); these cvars are its
// policy/tuning knobs, read back over JNI. The synthesized pad frame is injected
// into the guest below via ge::TouchPad (ge_touchpad.h).
//   ge_touch_controls : auto = show only when no gamepad is connected (default),
//                       on = always show, off = never show.
//   ge_touch_look_mode: swipe = drag the right half to look (default), stick =
//                       fixed right thumbstick.
REXCVAR_DEFINE_STRING(ge_touch_controls, "auto", "Input",
                      "On-screen touch controls: auto|on|off");
REXCVAR_DEFINE_STRING(ge_touch_look_mode, "stick", "Input",
                      "Touch look control: stick|swipe");
REXCVAR_DEFINE_DOUBLE(ge_touch_look_sens, 1.0, "Input",
                      "Touch look sensitivity").range(0.1, 8.0);
REXCVAR_DEFINE_DOUBLE(ge_touch_opacity, 0.5, "Input",
                      "Touch controls opacity (0..1)").range(0.1, 1.0);

// Runs once per controller poll, after XamInputGetState fills the slot-0 buffer
// and before the guest dispatches it. OR our keyboard buttons in, and set the
// left stick / triggers when their keys are held (pad input is preserved).
void ge_mouse_camera(uint8_t* base);  // defined above
void ge_apply_ce_data_patches(uint8_t* base);  // ge_ce_patches.cpp
// Phase-2 verification harness (defined below, next to the discovery hook, so
// it can share dbg_safe_ld32). See docs/HANDOFF-weapon-switch-direct-call.md.
bool ge_direct_equip(PPCContext* ctx, uint8_t* base, int32_t weapon_id);

// Hoisted up from the BeanTools CE MP hooks section below (this file's usual
// spot for it) because the direct-switch driver, earlier in the file, also
// needs it -- anonymous-namespace symbols are only visible from their point of
// declaration onward, and duplicating the constant in two anonymous
// namespaces would make it ambiguous at the later (CE hooks) use sites.
namespace { constexpr uint32_t GE_NET_FLAG = 0x830CAEA0u; }  // byte: !=0 = network MP session

void ge_inject_keyboard(PPCRegister& /*r11*/) {
  // Attach the input listener from the controller-poll path too. InitMouseLook()
  // runs at OnCreateDialogs when Runtime::display_window() can still be null, so
  // without this the listener never attaches while in menus and keyboard/mouse
  // stay dead until a level loads. This poll runs every frame (incl. menus), so
  // it attaches early. NB: the keyboard early-return moved BELOW the CE/mouse
  // block so data fixes + mouse-look still run when only the keyboard is off.
  ge_ensure_listener();
  PPCContext* ctx; uint8_t* base; getcb(ctx, base);

  // Apply BeanTools community DATA bug-fixes once, before any level loads its
  // setup/fog/BG data. The data segment is live in guest RAM by the first input
  // poll (menu), which precedes any level load.
  static bool ce_patched = false;
  if (!ce_patched) {
    ce_patched = true;
    ge_apply_ce_data_patches(base);
    REXKRNL_INFO("GECE community data bug-fixes applied");
  }

  // Mouse look runs every frame here, independent of the keyboard toggle. The
  // cross-platform listener only accumulates deltas while the game is focused and
  // the cursor is captured, so this is a no-op in menus / when unfocused.
  // tick_capture() drives the per-frame capture/recenter lifecycle (it used to
  // live in the now-removed ge_mouselook_pitch hook; ge_ensure_listener already
  // ran at the top of this function).
  g_listener.tick_capture();
  // Decay any wheel pulse armed by OnMouseWheel into key_down_ this frame (see
  // GameInputListener::tick_wheel()) -- must run once per frame, same cadence
  // the weapon-select block below polls kMouseWheelUp/Down at.
  g_listener.tick_wheel();
  if (REXCVAR_GET(ge_mouselook_enable)) ge_mouse_camera(base);

  // Phase-2 verification harness: an `equip <id>` posted from the memscan
  // command channel fires ONE direct switch, bypassing RequestEquipWeapon, so
  // the guest call can be exercised and observed in isolation. Diag-only.
  if (REXCVAR_GET(ge_gamestate_diag)) {
    const int32_t direct = ge::gamestate::TakeDirectEquip();
    if (direct != ge::gamestate::kNoWeapon) {
      if (ge::gamestate::GetWeaponSnapshot().valid) {
        ge_direct_equip(ctx, base, direct);
      } else {
        REXKRNL_INFO("GEWPN equip harness: no live player snapshot, dropped");
      }
    }
  }

  // Weapon actuation: move the game to the pending target posted via
  // RequestEquipWeapon. Preferred mechanism: direct calls into the game's own
  // switch routine (instant; see ge_direct_equip -- both hands, dual-aware).
  // The entry silently drops requests while the player is mid-action (e.g.
  // firing), so the driver re-issues every kDirectConfirm frames until the
  // switch lands or kMaxDirectTries expire. The pre-discovery Y-cycle walker
  // is kept intact behind ge_weapon_direct_switch=false as the escape hatch.
  {
    constexpr int kMaxSteps = 16;       // Y-cycle: total presses before giving up
    constexpr int kStepTimeout = 90;    // Y-cycle: frames for one switch to land
    constexpr int kDirectConfirm = 30;  // direct: frames between (re)issues
    constexpr int kMaxDirectTries = 10; // direct: re-issues before giving up
    static int steps = 0, wait = 0;
    static bool pressed = false;
    static int32_t equipped_at_press = 0;
    static int32_t last_target = ge::gamestate::kNoWeapon;
    static int direct_tries = 0, direct_wait = 0;
    const int32_t target = ge::gamestate::PeekEquipRequest();
    if (target == ge::gamestate::kNoWeapon) {
      steps = 0; wait = 0; pressed = false;
      direct_tries = 0; direct_wait = 0;
      last_target = ge::gamestate::kNoWeapon;
    } else {
      if (target != last_target) {
        // New target posted mid-flight: restart both mechanisms toward it.
        steps = 0; wait = 0; pressed = false;
        direct_tries = 0; direct_wait = 0;
        last_target = target;
      }
      const auto snap = ge::gamestate::GetWeaponSnapshot();
      const bool held = target >= 0 && target < ge::gamestate::kMaxWeaponSlots &&
                        (snap.held_mask & (1u << target)) != 0;
      if (!snap.valid || !held || snap.equipped_id == target) {
        // Done, or guarded out (no live inventory / unheld id) -- drop the request.
        ge::gamestate::ClearEquipRequest();
        steps = 0; wait = 0; pressed = false;
        direct_tries = 0; direct_wait = 0;
        last_target = ge::gamestate::kNoWeapon;
      } else if (REXCVAR_GET(ge_weapon_direct_switch) && base[GE_NET_FLAG] == 0) {
        // Direct calls act on GE_BONDVIEW_CUR-derived state, which cycles across
        // players in MP -- route MP sessions to the pad-injection walker below
        // (player-0-safe by construction). Local splitscreen is NOT covered by
        // this flag; see the handoff's known limitations.
        if (direct_wait > 0) {
          --direct_wait;  // waiting for the last issue to land
        } else if (direct_tries >= kMaxDirectTries) {
          REXKRNL_INFO("GEWPN direct switch gave up after {} tries (target={})",
                       direct_tries, target);
          ge::gamestate::ClearEquipRequest();
          direct_tries = 0; direct_wait = 0;
          last_target = ge::gamestate::kNoWeapon;
        } else {
          ge_direct_equip(ctx, base, target);  // dedups guest-side; re-issue is safe
          ++direct_tries; direct_wait = kDirectConfirm;
        }
      } else if (steps >= kMaxSteps) {
        ge::gamestate::ClearEquipRequest();
        steps = 0; wait = 0; pressed = false;
        last_target = ge::gamestate::kNoWeapon;
      } else if (pressed && snap.equipped_id == equipped_at_press && wait < kStepTimeout) {
        ++wait;  // Y-cycle: previous switch still in flight
      } else {
        // Y-cycle: first press, or the previous switch landed / timed out.
        ST16(base, GE_PAD0 + 0, LD16(base, GE_PAD0 + 0) | BTN_Y);
        equipped_at_press = snap.equipped_id;
        pressed = true; ++steps; wait = 0;
      }
    }
  }

  // On-screen touch controls: OR the Java overlay's synthesized pad frame into
  // the guest slot-0 buffer. Independent of ge_keyboard_enable / focus (a phone
  // has no keyboard and the overlay owns its own touches), so it runs before the
  // keyboard gate below. Only writes fields that are actually actuated, so it
  // never zeroes a real pad's input (the overlay is hidden when a pad is present
  // anyway -- see the auto policy in GoldenEyeActivity). No-op on desktop, where
  // TouchPad stays at its zeroed default.
  if (ge::TouchPad::Get().Active()) {
    const ge::PadState tp = ge::TouchPad::Get().GetState();
    if (tp.buttons)
      ST16(base, GE_PAD0 + 0, LD16(base, GE_PAD0 + 0) | tp.buttons);
    if (tp.left_trigger) base[GE_PAD0 + 2] = tp.left_trigger;
    if (tp.right_trigger) base[GE_PAD0 + 3] = tp.right_trigger;
    if (tp.thumb_lx) ST16(base, GE_PAD0 + 4, static_cast<uint16_t>(tp.thumb_lx));
    if (tp.thumb_ly) ST16(base, GE_PAD0 + 6, static_cast<uint16_t>(tp.thumb_ly));
    if (tp.thumb_rx) ST16(base, GE_PAD0 + 8, static_cast<uint16_t>(tp.thumb_rx));
    if (tp.thumb_ry) ST16(base, GE_PAD0 + 10, static_cast<uint16_t>(tp.thumb_ry));
  }

  if (!REXCVAR_GET(ge_keyboard_enable) || !ge_input_active()) return;

  uint16_t add = 0;
  if (ge_key_down("ge_key_a")) add |= BTN_A;
  if (ge_key_down("ge_key_b")) add |= BTN_B;
  if (ge_key_down("ge_key_x")) add |= BTN_X;
  if (ge_key_down("ge_key_y")) add |= BTN_Y;
  if (ge_key_down("ge_key_lb")) add |= BTN_LSHOULDER;
  if (ge_key_down("ge_key_rb")) add |= BTN_RSHOULDER;
  if (ge_key_down("ge_key_l3")) add |= BTN_LTHUMB;
  if (ge_key_down("ge_key_r3")) add |= BTN_RTHUMB;
  if (ge_key_down("ge_key_dup")) add |= BTN_DPAD_UP;
  if (ge_key_down("ge_key_ddown")) add |= BTN_DPAD_DOWN;
  if (ge_key_down("ge_key_dleft")) add |= BTN_DPAD_LEFT;
  if (ge_key_down("ge_key_dright")) add |= BTN_DPAD_RIGHT;
  if (ge_key_down("ge_key_start")) add |= BTN_START;
  if (ge_key_down("ge_key_back")) add |= BTN_BACK;
  if (add) ST16(base, GE_PAD0 + 0, LD16(base, GE_PAD0 + 0) | add);

  if (ge_key_down("ge_key_lt")) base[GE_PAD0 + 2] = 0xFF;
  if (ge_key_down("ge_key_rt")) base[GE_PAD0 + 3] = 0xFF;

  int16_t lx = 0, ly = 0;
  if (ge_key_down("ge_key_mv_left")) lx = -32767;
  if (ge_key_down("ge_key_mv_right")) lx = 32767;
  if (ge_key_down("ge_key_mv_up")) ly = 32767;
  if (ge_key_down("ge_key_mv_down")) ly = -32767;
  if (lx) ST16(base, GE_PAD0 + 4, static_cast<uint16_t>(lx));
  if (ly) ST16(base, GE_PAD0 + 6, static_cast<uint16_t>(ly));

  // Weapon selection: digits 1-9 jump straight to the Nth carried weapon, and
  // the scrollwheel steps next/prev through the carried list. Edge-triggered so
  // a held key posts exactly one request. Actuation happens in the driver
  // above (which walks or direct-calls the game to the target), so this block
  // only ever posts RequestEquipWeapon.
  if (REXCVAR_GET(ge_weapon_select_enable)) {
    static rex::ui::VirtualKey digit_vk[9] = {};
    static bool vk_init = false;
    if (!vk_init) {
      vk_init = true;
      const char* names[9] = {"1", "2", "3", "4", "5", "6", "7", "8", "9"};
      for (int i = 0; i < 9; ++i) digit_vk[i] = rex::ui::ParseVirtualKey(names[i]);
    }
    static uint16_t prev_digits = 0;
    static bool prev_next = false, prev_prev = false;
    const auto snap = ge::gamestate::GetWeaponSnapshot();
    if (snap.valid && snap.held_count > 0) {
      uint16_t digits = 0;
      for (int i = 0; i < 9; ++i)
        if (g_listener.key_down(digit_vk[i])) digits |= (uint16_t)(1u << i);
      for (int i = 0; i < 9 && i < snap.held_count; ++i)
        if ((digits & (1u << i)) && !(prev_digits & (1u << i)))
          ge::gamestate::RequestEquipWeapon(snap.held_ids[i]);
      prev_digits = digits;

      const bool next = ge_key_down("ge_key_wpn_next");
      const bool prev = ge_key_down("ge_key_wpn_prev");
      if ((next || prev) && REXCVAR_GET(ge_gamestate_diag))
        REXKRNL_INFO("GEWHEEL keydown next={} prev={}", next, prev);
      if ((next && !prev_next) || (prev && !prev_prev)) {
        int idx = 0;
        for (int i = 0; i < snap.held_count; ++i)
          if (snap.held_ids[i] == snap.equipped_id) { idx = i; break; }
        const int step = (next && !prev_next) ? 1 : -1;
        const int n = (idx + step + snap.held_count) % snap.held_count;
        ge::gamestate::RequestEquipWeapon(snap.held_ids[n]);
      }
      prev_next = next; prev_prev = prev;
    } else {
      prev_digits = 0; prev_next = false; prev_prev = false;
    }
  }
}

namespace {
// Fault-safe guest read for the diag hook below. The recomp's guard/unmapped
// pages make a raw LD32 of a stray guest pointer retry its SIGSEGV forever
// (total freeze -- hit live when the applier was called with a garbage r4
// during level load). pread on /proc/self/mem fails gracefully instead; same
// pattern as memscan::rd() in ge_gamestate.cpp. Returns 0xffffffff sentinel
// when unreadable.
uint32_t dbg_safe_ld32(uint8_t* base, uint32_t ga) {
#if defined(__linux__)
  static int fd = -1;
  if (fd < 0) {
    fd = ::open("/proc/self/mem", O_RDONLY);
    static bool warned = false;
    if (fd < 0 && !warned) {
      warned = true;
      REXKRNL_WARN("GEWPN dbg_safe_ld32: /proc/self/mem open failed; guest reads degrade to sentinel (dual detection disabled)");
    }
  }
  uint8_t b[4] = {0};
  if (fd < 0 || pread(fd, b, 4, (off_t)((uintptr_t)base + ga)) < 4) return 0xffffffffu;
  return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) |
         ((uint32_t)b[2] << 8) | (uint32_t)b[3];
#else
  (void)base; (void)ga;
  return 0xffffffffu;
#endif
}

// Dual-item inventory walk (task-3-analysis-round2.md "Host recipe" / Q1 & Q4):
// `g = load_be32(GE_BONDVIEW_CUR)` (0x82F1FAAC, the player-hands struct
// pointer already used elsewhere in this file) heads a circular linked list
// of carried-item records at `*(g+4744)`. Each node is
// `{u32 type @+0, u32 idA @+4, u32 idB @+8, u32 next @+12}`; `type==3` is a
// dual-item record `{3, idA, idB}` (a same-weapon dual, e.g. dual Phantoms,
// is `{3, 0x0c, 0x0c}`). Returns true and writes the *other* id in the node
// through `partner_out` iff a dual record containing `weapon_id` is found.
//
// Every read goes through dbg_safe_ld32 (fault-safe: returns the 0xffffffff
// sentinel instead of retrying a SIGSEGV forever) and every pointer is
// bounds-checked before it is dereferenced, mirroring
// ge_gamestate.cpp's plausible_guest_ptr idiom -- any sentinel/implausible
// read anywhere in the walk aborts it and reports "not dual" rather than
// risk a fault loop. kMaxDualWalk bounds the node count as a second guard
// against a corrupt/cyclic list that never re-hits `head`.
constexpr int kMaxDualWalk = 64;
inline bool ge_plausible_guest_ptr(uint32_t ga) {
  return ga >= 0x00010000u && ga < 0xFFFF0000u;
}
bool ge_find_dual_partner(uint8_t* base, int32_t weapon_id, int32_t* partner_out) {
  const uint32_t g = dbg_safe_ld32(base, GE_BONDVIEW_CUR);
  if (g == 0xffffffffu || !ge_plausible_guest_ptr(g)) return false;
  const uint32_t head = dbg_safe_ld32(base, g + 4744u);
  if (head == 0xffffffffu || head == 0u || !ge_plausible_guest_ptr(head)) return false;
  uint32_t node = head;
  for (int i = 0; i < kMaxDualWalk; ++i) {
    const uint32_t type = dbg_safe_ld32(base, node + 0u);
    const uint32_t idA  = dbg_safe_ld32(base, node + 4u);
    const uint32_t idB  = dbg_safe_ld32(base, node + 8u);
    const uint32_t next = dbg_safe_ld32(base, node + 12u);
    if (type == 0xffffffffu || idA == 0xffffffffu || idB == 0xffffffffu ||
        next == 0xffffffffu) {
      return false;  // fault mid-walk -- never trust a torn read
    }
    if (type == 3u && (static_cast<int32_t>(idA) == weapon_id ||
                        static_cast<int32_t>(idB) == weapon_id)) {
      *partner_out = static_cast<int32_t>(
          static_cast<int32_t>(idA) == weapon_id ? idB : idA);
      return true;
    }
    if (next == 0u || next == head) break;       // list end / one full loop
    if (!ge_plausible_guest_ptr(next)) break;     // implausible next -- stop
    node = next;
  }
  return false;
}
}  // namespace

// ===========================================================================
// Phase-1 discovery hook (weapon direct-switch RE; spec:
// docs/superpowers/specs/2026-07-03-weapon-direct-switch-design.md). Entry of
// sub_820A7508, the per-hand weapon applier -- the known bottom of the switch
// call chain. Logs the guest caller (lr) and args so a pause-menu inventory
// selection can be diffed against a Y-cycle switch: the first lr unique to
// the pause-menu path identifies the direct-switch caller. Inert unless
// ge_gamestate_diag is set (desktop RE sessions only).
// ===========================================================================
void ge_dbg_weapon_apply(PPCRegister& r3, PPCRegister& r4) {
  if (!REXCVAR_GET(ge_gamestate_diag)) return;
  PPCContext* ctx; uint8_t* base; getcb(ctx, base);
  const uint32_t obj = r4.u32;
  uint32_t w[4] = {0, 0, 0, 0};
  if (obj) for (int i = 0; i < 4; ++i) w[i] = dbg_safe_ld32(base, obj + 4u * i);
  // 0x447f10b0 = equipped-id block (kEquipIdAddr in ge_gamestate.cpp); reads
  // are fault-safe (dbg_safe_ld32) -- the applier can be called with a
  // garbage r4 during level load, so "guaranteed live" was a false assumption
  // that caused a total freeze (raw LD32 retrying a SIGSEGV forever).
  REXKRNL_INFO("GEWPNAPPLY lr={:#010x} hand={} obj={:#010x} "
               "obj[0..3]={:#010x},{:#010x},{:#010x},{:#010x} equip={}",
               (uint32_t)ctx->lr, r3.u32, obj, w[0], w[1], w[2], w[3],
               (int32_t)dbg_safe_ld32(base, 0x447f10b0u));
}

// ===========================================================================
// Phase-2 verification harness: direct weapon switch -- the verified
// two-hand pair-call recipe (task-3-analysis-round2.md "Host recipe"). The
// native Y-cycle helpers never issue a lone hand-0 or hand-1 request; they
// always request BOTH hands together in the same quantum (sub_820A6F70(0,...)
// immediately followed by sub_820A6F70(1,...)), which is what arms hand 1's
// state-5 bypass before hand 0's own swap point can force-clobber it (round-2
// Q1's "ordering/clobber caveat"). This function reproduces that pairing:
//   - Single switch: hand 0 <- weapon_id, hand 1 <- 0 (id 0 is an explicit
//     off-hand clear -- sub_820C0D48's "b==0" rule always passes it), so a
//     single-weapon switch also clears any stale off-hand weapon.
//   - Dual grant: if the inventory's dual-item list (walked fault-safe by
//     ge_find_dual_partner) has a {3, idA, idB} record containing weapon_id,
//     hand 1 <- the other id in that record (usually == weapon_id), granting
//     both hands the dual weapon; otherwise hand 1 still gets the explicit
//     clear above.
// Always issues the pair and returns true -- the pair-call has no failure
// mode of its own (sub_820A6F70 has no validation, see round-2 Q2). The bool
// return is kept for a future compile-out safety switch, not because this
// can fail today.
//
// MUST run on a guest thread inside a midasm hook. The full-context
// save/restore is required: we are mid-way through ge_inject_keyboard's host
// hook, and the generated caller resumes from ctx after we return -- a guest
// call here clobbers volatile registers/lr otherwise. One save covers both
// calls; there must be no return between them (same-quantum requirement).
// ===========================================================================
bool ge_direct_equip(PPCContext* ctx, uint8_t* base, int32_t weapon_id) {
  const int32_t before = (int32_t)dbg_safe_ld32(base, 0x447f10b0u);  // equipped-id block

  int32_t partner = 0;
  const bool dual = ge_find_dual_partner(base, weapon_id, &partner);
  const int32_t off_hand_id = dual ? partner : 0;

  const PPCContext saved = *ctx;

  // sub_820A6F70(hand, weapon id, direction/mode) -- the sole funnel for the
  // native Y-cycle input paths (Findings 2026-07). Both calls land in this
  // one hook invocation, back-to-back, so hand 1's requested-state is armed
  // before hand 0's swap point runs.
  ctx->r3.u32 = 0u;                        // hand 0 = main hand
  ctx->r4.u32 = (uint32_t)weapon_id;       // raw weapon id (verified: applier takes ids)
  ctx->r5.u32 = 1u;                        // direction/mode; constant 1 on all native paths
  sub_820A6F70(*ctx, base);

  ctx->r3.u32 = 1u;                        // hand 1 = off hand
  ctx->r4.u32 = (uint32_t)off_hand_id;     // dual partner id, or 0 = explicit clear
  ctx->r5.u32 = 1u;
  sub_820A6F70(*ctx, base);

  *ctx = saved;
  REXKRNL_INFO("GEWPN direct equip id={} dual={} equip_before={} equip_after={}",
               weapon_id, dual, before, (int32_t)dbg_safe_ld32(base, 0x447f10b0u));
  return true;
}

// ===========================================================================
// BeanTools Community Edition CODE fixes (instruction patches replicated as
// midasm hooks; the recomp runs generated C++ so the xex bytes can't be patched
// directly). Addresses/values 1:1 with finalizer.c. Data-only CE fixes live in
// ge_ce_patches.cpp.
// ===========================================================================

// fix_door_volume_clamp @0x820DD814: `li r3,0` -> `li r3,1` (min volume for
// distant doors; 0 overflows). After-hook forces r3 = 1.
void ge_ce_door_vol(PPCRegister& r3) { r3.u32 = 1; }

// remove_beta_string_at_logo @0x820ED678: `ori r3,r3,0x9D97` -> `...0x9CE3`
// (point the GoldenEye-logo string id at the empty string). Replace low half.
void ge_ce_beta_str(PPCRegister& r3) {
  r3.u32 = (r3.u32 & 0xFFFF0000u) | 0x9CE3u;
}

// extend_audio_distance, store site 0x8214438C: original `stfs f0,0x5C(r31)`
// stored a small default scaler; CE makes X3DEmitter->CurveDistanceScaler =
// 6500.0f. Re-store 6500.0f (0x45CB2000) to r31+0x5C after the original store.
void ge_ce_audio_dist(PPCRegister& r31) {
  PPCContext* ctx; uint8_t* base; getcb(ctx, base); (void)ctx;
  ST32(base, r31.u32 + 0x5Cu, 0x45CB2000u);  // 6500.0f
}

// hardcode_near_clip_to_2, per-fog store site 0x82117B44: original
// `stfs f0,0x14(r11)` writes the fog entry's near-clip into the global. CE NOOPs
// it and pins the global to 2.0f. Can't NOOP a store in the recomp, so re-write
// the just-stored slot (r11+0x14 == the near-clip global) back to 2.0f each load.
void ge_ce_near_clip(PPCRegister& r11) {
  PPCContext* ctx; uint8_t* base; getcb(ctx, base); (void)ctx;
  ST32(base, r11.u32 + 0x14u, 0x40000000u);  // 2.0f
}

// remove_original_graphics_mode_blur @0x82188E70: CE NOOPs `bne cr6,+0x19C` so
// the blur path is never taken. Branch-replace -> always fall through.
//   jump_on_true  = 0x8218900C (original target, never taken)
//   jump_on_false = 0x82188E74 (fall through)
bool ge_ce_blur(PPCRegister& /*r3*/) { return false; }

// remove_original_graphics_mode_from_intro @0x8209972C: CE turns
// `bne cr6,0x82099750` into an unconditional `b 0x82099750` so the intro reads
// the current graphics-mode flag. Branch-replace -> always take.
//   jump_on_true  = 0x82099750 (always)
//   jump_on_false = 0x82099730 (unused)
bool ge_ce_intro_gfx(PPCRegister& /*r3*/) { return true; }

// ===========================================================================
// BeanTools Community Edition MP / network hack-functions, re-implemented as
// midasm hooks (the recomp can't add the new 0x830E guest code, so each hack's
// logic is replicated in C++ -- the same pattern as ge_hook_830E0xxx). Game
// functions are called directly via their generated sub_ symbols. 1:1 with
// finalizer.c.
// ===========================================================================
// GE_NET_FLAG (byte @0x830CAEA0u, !=0 = network MP session) is declared above,
// near ge_inject_keyboard, which also needs it.

// disable_doors_autoclosing_on_mp @0x820E4F1C (after `lwz r11,0xE8(r30)` loads
// the door open-tick): in a network session, force it to 0 so doors never
// auto-close. Outside a session, keep the loaded value.
void ge_ce_mp_door(PPCRegister& r11) {
  PPCContext* ctx; uint8_t* base; getcb(ctx, base); (void)ctx;
  if (base[GE_NET_FLAG] != 0) r11.u32 = 0;
}

// disable_player_collisions_for_network_mp @0x820CDFA4 (replaces `bl sub_820B3E90`,
// the player-collision-radius calc): run it normally outside a network session;
// in one, skip it so players pass through each other. The CE hack tail-returns
// from the enclosing function in both cases, so return=true.
void ge_ce_mp_collision() {
  PPCContext* ctx; uint8_t* base; getcb(ctx, base);
  if (base[GE_NET_FLAG] == 0) sub_820B3E90(*ctx, base);
}

// fix_golden_gun_respawn_visiblity_flag @0x820CF940 (replaces cmpwi/bne): keep
// the respawning weapon's invisible flag only for the golden gun in the MWTGG
// scenario (so it stays hidden until grabbed); otherwise clear it so weapons
// reappear. MP scenario id @0x82F61084; weapon id in r11 (golden gun = 0x13).
//   jump_on_true  = 0x820CF948 (keep invisible: GG path)
//   jump_on_false = 0x820CF94C (clear flag: normal path)
bool ge_ce_golden_gun(PPCRegister& r11) {
  PPCContext* ctx; uint8_t* base; getcb(ctx, base); (void)ctx;
  return LD32(base, 0x82F61084u) == 3u && r11.u32 == 0x13u;
}

// make_mp_always_use_p2_fog @0x82117CB0 (after `cmpwi cr6,r3,1` @0x82117CAC; r3 =
// active player count): with 2+ players, force the fog index to 2 (P2 fog) for a
// consistent look; 1 player keeps the original path.
//   jump_on_true  = 0x82117CB8 (2+ players: continue with r3=2)
//   jump_on_false = 0x82117CB4 (1 player: original `li r3,0`)
bool ge_ce_p2_fog(PPCRegister& r3) {
  if (r3.s32 != 1) { r3.u32 = 2; return true; }
  return false;
}

// fix_network_armor_bug @0x8216BC1C (after `stw r12,0x64(r30)`): re-implements
// the CE `cal_dam` armor hack (the patch ships its C source). When an armor prop
// is processed, award it to the NEAREST player within 10m -- fixes armor not
// being granted to remote players in network MP. r30 = armor prop pointer.
// Offsets from armor_fix_code.h: prop.type@+3, prop.pos@+0x58, prop.armorval@+0x84;
// player coords ptr@+0x1AC, coord.pos@+0xC, player.armor@+0x1E8. Float consts:
// 50.0@0x82000B90, 1000.0@0x8200371C (=10m), 1e6@0x82003F0C.
void ge_ce_armor_fix(PPCRegister& r30) {
  PPCContext* ctx; uint8_t* base; getcb(ctx, base); (void)ctx;
  const uint32_t prop = r30.u32;
  if (base[prop + 3] != 0x15) return;  // not an armor prop
  const float f50 = LDF32(base, 0x82000B90u);
  const float f1000 = LDF32(base, 0x8200371Cu);
  float nearest = LDF32(base, 0x82003F0Cu);  // 1,000,000
  const float px = LDF32(base, prop + 0x58u);
  const float py = LDF32(base, prop + 0x5Cu);
  const float pz = LDF32(base, prop + 0x60u);
  int pick = -1;
  for (int i = 0; i < 4; ++i) {
    const uint32_t pl = LD32(base, 0x82F1FA98u + i * 4u);
    if (!pl) continue;
    const uint32_t coords = LD32(base, pl + 0x1ACu);
    const float dx = LDF32(base, coords + 0x0Cu) - px;
    const float dy = (LDF32(base, coords + 0x10u) - f50) - py;
    const float dz = LDF32(base, coords + 0x14u) - pz;
    const float test = sqrtf(dx * dx + dy * dy + dz * dz);
    if (test < nearest) { nearest = test; pick = i; }
  }
  if (pick < 0 || nearest > f1000) return;  // nearest player >10m away (or none)
  const uint32_t winner = LD32(base, 0x82F1FA98u + pick * 4u);
  STF32(base, winner + 0x1E8u, LDF32(base, prop + 0x84u));  // grant armorval
}

// increase_mp_characters: bump the unlocked MP character count from 0x21 to 0x32
// (`li r11,0x21` -> `li r11,0x32`) at the two unlock sites (0x820EF350 SP-clear,
// 0x82106C54 system-link). The new character struct data is written in
// ge_ce_patches.cpp (mpchars_altsandbonus -> 0x8272BA80).
void ge_ce_mp_charcount(PPCRegister& r11) { r11.u32 = 0x32u; }

// add_sfx_to_remote_player_weapons @0x8216E25C (runs BEFORE the original
// `add r11,r10,r11`): play the firing SFX for a REMOTE player's weapon so you
// hear other players shoot online. The CE hack saved/restored every register
// around the SFX calls; we snapshot/restore the whole PPC context so the
// remote-fire (tracer-spawn) function continues undisturbed -- the SFX is a pure
// side effect. r11 = remote player struct pointer.
//   paused flag @0x830633EC; remote-fire gate player+0x2044; old sound-buffer
//   slots player+0xAFC / +0xB00; current weapon player+0x928; weapon-stats array
//   @0x82421968 stride 0x38 (model flag +0x08, stats ptr +0x0C, sound id +0x26);
//   play=sub_82144920, free=sub_82144970/sub_82144A08, set-loc=sub_821448F8;
//   solo-fullscreen screen flag @0x8272B424; player coords player+0x1AC (+0xC).
void ge_ce_remote_weapon_sfx(PPCRegister& r11) {
  PPCContext* ctx; uint8_t* base; getcb(ctx, base);
  const uint32_t player = r11.u32;
  if (!player) return;
  if (LD32(base, 0x830633ECu) != 0) return;        // game paused
  if (LD32(base, player + 0x2044u) != 0) return;   // remote-fire gate

  PPCContext saved = *ctx;  // the SFX calls clobber volatile regs; restore after

  auto deactivate = [&](uint32_t slot) {
    uint32_t buf = LD32(base, slot);
    if (!buf) return;
    ctx->r3.u32 = buf; sub_82144970(*ctx, base);
    if (ctx->r3.u32 == 0) return;
    ctx->r3.u32 = LD32(base, slot); sub_82144A08(*ctx, base);
  };
  deactivate(player + 0x0AFCu);   // free old sound buffer 1
  deactivate(player + 0x0B00u);   // free old sound buffer 2

  const uint32_t snd_slot = player + 0x0B00u;  // play into buffer-2 slot
  const uint32_t channel  = 0x1461u;

  const uint32_t weapon = LD32(base, player + 0x928u);
  const uint32_t entry  = 0x82421968u + weapon * 0x38u;  // weapon stats entry
  if (LD32(base, entry + 0x08u) != 0) { *ctx = saved; return; }  // no model -> no sfx
  const uint32_t stats = LD32(base, entry + 0x0Cu);
  if (stats == 0) { *ctx = saved; return; }                     // null stats
  const uint32_t sound_id = LD16(base, stats + 0x26u);          // weapon sound id
  if ((int32_t)sound_id > 0x105) { *ctx = saved; return; }      // illegal range

  ctx->r3.u32 = LD32(base, 0x83064DE0u);
  ctx->r4.u32 = sound_id;
  ctx->r5.u32 = snd_slot;
  ctx->r6.u32 = LD32(base, 0x83064DE8u);
  ctx->r7.u32 = 0x820036A8u;
  ctx->r8.u32 = channel;
  sub_82144920(*ctx, base);          // play sfx -> r3 = sound buffer
  const uint32_t buf = ctx->r3.u32;
  if (buf != 0 && LD32(base, 0x8272B424u) == 3u) {  // solo full-screen -> 3D pos
    const uint32_t coord = LD32(base, player + 0x01ACu);
    ctx->r3.u32 = buf;
    ctx->r4.u32 = coord + 0x0Cu;
    sub_821448F8(*ctx, base);        // set 3D location
  }

  *ctx = saved;  // restore -> remote-fire function continues unaffected
}

// set_mp_sfx_to_use_player_location: the 4 SFX call sites (gasp 0x820BF264,
// slapper 0x820CDC5C, knife 0x820ACF54, item-equip 0x820AC4D0) all originally
// `bl sub_82144920` (play sfx). CE redirects each through a helper that plays the
// sound AND positions it at the emitting player's 3D location, so in
// split-screen-solo/online you hear other players' actions directionally. This
// hook IS that helper: it plays the sfx (args already in ctx from the caller),
// then sets the 3D location -- but only when another player is the source (not
// the local/active-viewport player, whose own sounds stay centered). Registered
// at all 4 sites with jump_address = site+4 to replace the original bl. No reg
// save needed: the original was itself a bl, so volatiles are already clobbered.
// Shared helper: play the sfx (args already in ctx) and 3D-position it at the
// emitting player -- but only when the source is a NON-local player (the local/
// active-viewport player's own sounds stay centered).
static void ge_ce_play_at_location(PPCContext* ctx, uint8_t* base) {
  sub_82144920(*ctx, base);                    // play sfx (caller's args in ctx)
  const uint32_t buf = ctx->r3.u32;            // sound buffer handle
  if (buf == 0) return;                        // null buffer -> done
  if (LD32(base, 0x8272B424u) != 3u) return;   // not solo full-screen view
  if (LD32(base, 0x82F1FA9Cu) == 0 && LD64(base, 0x82F1FAA0u) == 0)
    return;                                    // single-player -> no positioning
  const uint32_t cur = LD32(base, 0x82F1FAACu);  // current player
  if (LD32(base, cur + 0x904u) == 0) return;   // local active viewport -> centered
  const uint32_t coord = LD32(base, cur + 0x1ACu);
  ctx->r3.u32 = buf;
  ctx->r4.u32 = coord + 0x0Cu;                 // -> player world location
  sub_821448F8(*ctx, base);                    // set 3D location
  ctx->r3.u32 = buf;                           // leave buffer in r3 for downstream
}

void ge_ce_sfx_3d() {
  PPCContext* ctx; uint8_t* base; getcb(ctx, base);
  ST32(base, 0x824203ACu, 0);  // your own sound -> your death -> death tune plays
  ge_ce_play_at_location(ctx, base);
}

// Trigger Gasps If Local Player Damaged, Else Argh (set_mp_sfx, gasp half) @
// 0x820BF408 (replaces `bl sub_82144920`). When the damaged player is a REMOTE
// player (solo-fullscreen + MP + not the local viewport + has a model), play a
// gender-appropriate "argh" at their 3D location and flag this death as NOT
// yours (0x824203AC=1) so the death tune is suppressed for it; otherwise it's
// your own gasp (flag=0 -> death tune plays). jump_address skips the original bl.
//   chr = player+0x1AC, model = chr+0x08, bodynum = model+0x0F; body-info array
//   0x82729020 stride 0x24, gender +0x18; argh index female 0x83062BF4 (0..2,
//   +0x0D) / male 0x83062BF8 (0..0x18, +0x86).
void ge_ce_gasp() {
  PPCContext* ctx; uint8_t* base; getcb(ctx, base);
  uint32_t model = 0;
  if (LD32(base, 0x8272B424u) == 3u) {                       // solo full-screen
    const bool mp = (LD32(base, 0x82F1FA9Cu) != 0) || (LD64(base, 0x82F1FAA0u) != 0);
    if (mp) {
      const uint32_t cur = LD32(base, 0x82F1FAACu);
      if (LD32(base, cur + 0x904u) != 0) {                   // not the local viewport
        const uint32_t chr = LD32(base, cur + 0x1ACu);
        if (chr) model = LD32(base, chr + 0x08u);
      }
    }
  }
  if (model != 0) {                                          // remote player damaged
    ST32(base, 0x824203ACu, 1u);                             // not your death
    const uint32_t bodynum = base[model + 0x0Fu];
    const uint32_t gender = base[0x82729020u + bodynum * 0x24u + 0x18u];
    ctx->r5.u32 = 0;
    uint32_t arghid;
    if (gender == 0u) {                                      // female
      int32_t i = (int32_t)LD32(base, 0x83062BF4u) + 1;
      if (i > 2) i = 0;
      ST32(base, 0x83062BF4u, (uint32_t)i);
      arghid = (uint32_t)i + 0x0Du;
    } else {                                                 // male
      int32_t i = (int32_t)LD32(base, 0x83062BF8u) + 1;
      if (i > 0x18) i = 0;
      ST32(base, 0x83062BF8u, (uint32_t)i);
      arghid = (uint32_t)i + 0x86u;
    }
    ctx->r4.u32 = arghid;
    ge_ce_play_at_location(ctx, base);                       // argh at their location
  } else {                                                   // your own gasp
    ST32(base, 0x824203ACu, 0u);                             // your death -> death tune plays
    sub_82144920(*ctx, base);                                // play gasp (caller's args)
  }
}

// only_trigger_mp_death_tune_for_your_kills_and_yourself @0x820BFB04 (the
// `bl <play death tune>`; r3 already = 6 from the vanilla `li r3,6` at 0x820BFB00).
// Skip the death tune when 0x824203AC is set (the gasp hook flagged this death as
// another player's). jump_on_true skips the bl; no jump_on_false -> falls through
// and plays it for your own deaths.
bool ge_ce_death_tune() {
  PPCContext* ctx; uint8_t* base; getcb(ctx, base); (void)ctx;
  return LD32(base, 0x824203ACu) != 0u;
}

// reset_internal_cheat_state float relocation @0x8209D88C: the death-tune logic
// reads a 5.0f that originally lived at the address CE now repurposes as the
// bypass flag (0x824203AC). After the original `lfs f1,0x3AC(r11)`, reload f1
// from +0x3A8 instead (where ge_ce_patches stashes the 5.0f). r11 = 0x82420000.
void ge_ce_killtune_float(PPCRegister& r11, PPCRegister& f1) {
  PPCContext* ctx; uint8_t* base; getcb(ctx, base); (void)ctx;
  f1.f64 = (double)LDF32(base, r11.u32 + 0x3A8u);
}

// fix_watch_volume_sliders_range: the watch volume sliders only spanned half the
// real 0-100 range. CE reads the stored byte and halves it for display, and
// doubles the slider value before storing (entering the save routine past its
// clamp). READ hooks replace `bl <vol read>` (r3 = settings ptr -> vol byte >> 1);
// SAVE hooks replace `bl <vol save>` (double r4, then run the save routine from
// its mid-point continuation so the doubled value isn't clamped back). Music vol
// byte = settings+0x295, fx vol = settings+0x294. All use jump_address = site+4.
void ge_ce_watch_music_read(PPCRegister& r3) {
  PPCContext* ctx; uint8_t* base; getcb(ctx, base); (void)ctx;
  r3.u32 = (uint32_t)(base[r3.u32 + 0x295u] >> 1);
}
void ge_ce_watch_sfx_read(PPCRegister& r3) {
  PPCContext* ctx; uint8_t* base; getcb(ctx, base); (void)ctx;
  r3.u32 = (uint32_t)(base[r3.u32 + 0x294u] >> 1);
}
void ge_ce_watch_music_save() {
  PPCContext* ctx; uint8_t* base; getcb(ctx, base);
  ctx->r4.u32 = ctx->r4.u32 + ctx->r4.u32;   // double the slider value
  ge_cont_82184E18(*ctx, base);              // save routine past its clamp
}
void ge_ce_watch_sfx_save() {
  PPCContext* ctx; uint8_t* base; getcb(ctx, base);
  ctx->r4.u32 = ctx->r4.u32 + ctx->r4.u32;
  ge_cont_82184E48(*ctx, base);
}
