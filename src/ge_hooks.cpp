// ge - project mid-ASM hooks: 0x830E0xxx fragment reconstruction.
#include <thread>
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
#include <rex/cvar.h>  // REXCVAR_DEFINE_BOOL / REXCVAR_GET (ge_freeze_diag)
#include <rex/hook.h>  // ThreadState, kernel_state, memory
#include <rex/runtime.h>
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

void ge_watchdog_thread() {
  uint8_t* base = rex::system::kernel_state()->memory()->virtual_membase();
  uint32_t last_wpi = 0xFFFFFFFFu, last_rpi = 0, last_present = 0, last_submit = 0;
  uint32_t present_at_stall_start = 0, dbg_at_stall_start = 0, submit_at_stall_start = 0;
  uint32_t stall = 0;
  bool logged = false;
  bool recover_fired = false;
  for (;;) {
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    auto* cp = ge_cp();
    if (!cp) continue;
    uint32_t wpi = static_cast<CPProbe*>(cp)->wpi();
    uint32_t rpi = static_cast<CPProbe*>(cp)->rpi();
    uint32_t present = g_present_cpcnt.load(std::memory_order_relaxed);
    uint32_t dbg = g_dbgnow_calls.load(std::memory_order_relaxed);
    uint32_t dev = g_ge_device.load(std::memory_order_relaxed);
    uint32_t idblk = g_ge_idblk.load(std::memory_order_relaxed);
    uint32_t submit = dev ? LD32(base, dev + 16544) : 0;

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
      ge::FpsOnFrame();  // one real rendered frame -> feed the FPS benchmark recorder
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
  PPCContext* ctx; uint8_t* base; getcb(ctx, base); (void)ctx; (void)base;
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

namespace {
// Direct 1:1 look: mouse delta -> absolute yaw/pitch, written straight into the
// camera state every frame (tune via sens).
constexpr float kYawDegPerCount = 0.10f;       // guest yaw DEGREES per mouse count @ sens 1
constexpr float kPitchPerCount = 0.10f;        // guest pitch (+612) units per mouse count @ sens 1
constexpr float kPitchMin = -60.0f;
constexpr float kPitchMax = 60.0f;
constexpr uint32_t GE_BONDVIEW_CUR = 0x82F1FAACu; // dword_82F1FAAC -> current bondview struct
constexpr uint32_t GE_CUTSCENE = 0x82F1F8DCu;     // gBondViewCutscene ptr; non-zero = cutscene/non-playable
constexpr uint32_t GE_CAMERA_MODE = 0x82F1F920u;  // g_CameraMode. 1/2/3=intro fly-in, 4=FP playable
constexpr uint32_t GE_CAM_FP = 4u;                // first-person playable mode (only state the player aims)
constexpr uint32_t GE_BV_YAW = 596u;              // +596 facing yaw (degrees, HUD/radar copy)
constexpr uint32_t GE_BV_PITCH = 612u;            // +612 pitch angle
constexpr uint32_t GE_BV_CENTER_FLAG = 524u;      // +524 auto-center-this-frame flag
// Camera yaw is built from the heading -- drive it + its smoother steady-state.
constexpr uint32_t GE_YAW_HEADING = 0x82F1F900u;  // flt_82F1F900 heading (rad)
constexpr uint32_t GE_YAW_SMOOTH = 0x82F1F904u;   // flt_82F1F904 smoothed (= heading*12.5)
constexpr uint32_t GE_YAW_TARGET = 0x82F1F910u;   // flt_82F1F910 target heading (rad)
constexpr uint32_t GE_YAW_OFFSET = 0x82F1F8F0u;   // flt_82F1F8F0 yaw offset added to heading
constexpr float GE_TWO_PI = 6.2831855f;
constexpr float GE_RAD2DEG = 57.29578f;
}  // namespace

namespace {
float g_look_yaw_deg = 0.0f;  // our absolute yaw (deg, [0,360))
float g_look_pitch = 0.0f;    // our absolute pitch (+612 units)
uint32_t g_look_bv = 0;       // bondview struct we're tracking
}  // namespace

REXCVAR_DEFINE_DOUBLE(ge_mouse_sens, 1.0, "Input", "Mouse look sensitivity").range(0.05, 20.0);
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

  // Per-frame consumption by the look hook (reset-on-read).
  int take_dx() { std::lock_guard<std::mutex> l(m_); int v = dx_; dx_ = 0; return v; }
  int take_dy() { std::lock_guard<std::mutex> l(m_); int v = dy_; dy_ = 0; return v; }

  bool key_down(rex::ui::VirtualKey vk) const {
    uint16_t idx = static_cast<uint16_t>(vk);
    if (idx >= 256) return false;
    std::lock_guard<std::mutex> l(m_);
    return key_down_[idx];
  }

  bool focused() const { return window_ && window_->HasFocus(); }
  bool suppressed() const { return suppressed_.load(std::memory_order_relaxed); }

  void set_suppressed(bool v) {
    suppressed_.store(v, std::memory_order_relaxed);
    if (v) { std::lock_guard<std::mutex> l(m_); dx_ = 0; dy_ = 0; }  // drop queued motion
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
      dx_ = 0; dy_ = 0; have_prev_ = false;
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
    const int x = e.x(), y = e.y();
    if (have_prev_) { dx_ += x - prev_x_; dy_ += y - prev_y_; }
    prev_x_ = x; prev_y_ = y; have_prev_ = true;
  }
  void OnMouseDown(rex::ui::MouseEvent& e) override { set_mouse_button(e.button(), true); }
  void OnMouseUp(rex::ui::MouseEvent& e) override { set_mouse_button(e.button(), false); }
  void OnKeyDown(rex::ui::KeyEvent& e) override { set_key(e.virtual_key(), true); }
  void OnKeyUp(rex::ui::KeyEvent& e) override { set_key(e.virtual_key(), false); }

  // WindowListener: drop held keys / queued motion on focus loss (no stuck keys
  // or view snap on alt-tab). Capture itself auto-releases via tick_capture.
  void OnLostFocus(rex::ui::UISetupEvent&) override {
    std::lock_guard<std::mutex> l(m_);
    std::memset(key_down_, 0, sizeof(key_down_));
    dx_ = 0; dy_ = 0; have_prev_ = false;
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

  mutable std::mutex m_;     // guards dx_/dy_/prev_*/have_prev_/key_down_
  std::mutex cap_m_;         // serializes capture transitions (captured_)
  rex::ui::Window* window_ = nullptr;
  int dx_ = 0, dy_ = 0;
  int prev_x_ = 0, prev_y_ = 0;
  bool have_prev_ = false;
  bool captured_ = false;
  std::atomic<bool> suppressed_{false};  // true while the pause menu is open
  bool key_down_[256] = {};
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

int ge_take_mouse_dx() { return g_listener.take_dx(); }
int ge_take_mouse_dy() { return g_listener.take_dy(); }
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

// YAW: this stick-yaw write site only runs in control mode 2, which normal
// gameplay does NOT use -- so mouse YAW is applied in ge_mouselook_pitch instead
// (that site runs every frame). Kept only to confirm firing / lazily attach.
void ge_mouselook_yaw(PPCRegister& /*r11*/) { ge_ensure_listener(); }

// PITCH: runs every frame in ge_bondview_control (unconditional pitch site).
// Keep our own absolute yaw/pitch from raw mouse deltas and write them straight
// into the camera state: +596 (HUD yaw), +612 (pitch), and the heading
// (flt_82F1F900/904/910) the view matrix is built from. Re-sync to the game's
// values when the active view changes.
void ge_mouselook_pitch(PPCRegister& /*r11*/) {
  ge_ensure_listener();
  g_listener.tick_capture();  // capture lifecycle (matches upstream: independent of FP gate)
  PPCContext* ctx; uint8_t* base; getcb(ctx, base); (void)ctx;
  uint32_t bv = LD32(base, GE_BONDVIEW_CUR);
  if (!bv) return;

  // Suspend mouse-look whenever the player isn't in first-person control:
  //   * g_CameraMode != FP  -> level-start intro fly-in (modes 1/2/3), swirl,
  //     death cam, ending pose, fade-to-title, etc. (gameplay = 4).
  //   * gBondViewCutscene   -> a scripted camera took over mid-level in FP mode.
  // Clear g_look_bv so the view re-syncs to the game's angle when control returns
  // (no snap) instead of fighting the scripted/intro camera.
  if (!ge_mouselook_on() || LD32(base, GE_CAMERA_MODE) != GE_CAM_FP ||
      LD32(base, GE_CUTSCENE) != 0) {
    g_look_bv = 0;
    return;
  }

  float game_yaw = LDF32(base, bv + GE_BV_YAW);
  float game_pitch = LDF32(base, bv + GE_BV_PITCH);
  if (bv != g_look_bv) {  // new view -> adopt the game's current angles
    g_look_bv = bv;
    g_look_yaw_deg = game_yaw;
    g_look_pitch = game_pitch;
  }

  const float sens = static_cast<float>(REXCVAR_GET(ge_mouse_sens));
  int dx = ge_take_mouse_dx();
  int dy = ge_take_mouse_dy();

  g_look_yaw_deg += static_cast<float>(dx) * sens * kYawDegPerCount;   // right = +yaw
  while (g_look_yaw_deg >= 360.0f) g_look_yaw_deg -= 360.0f;
  while (g_look_yaw_deg < 0.0f) g_look_yaw_deg += 360.0f;

  g_look_pitch += static_cast<float>(-dy) * sens * kPitchPerCount;     // up = look up
  if (g_look_pitch > kPitchMax) g_look_pitch = kPitchMax;
  if (g_look_pitch < kPitchMin) g_look_pitch = kPitchMin;

  STF32(base, bv + GE_BV_YAW, g_look_yaw_deg);   // HUD/radar copy
  STF32(base, bv + GE_BV_PITCH, g_look_pitch);
  ST32(base, bv + GE_BV_CENTER_FLAG, 0u);  // suppress vertical auto-center

  // YAW: drive the heading the camera is built from (+ its smoother steady-state).
  float offset = LDF32(base, GE_YAW_OFFSET);
  float h = g_look_yaw_deg / GE_RAD2DEG - offset;
  while (h >= GE_TWO_PI) h -= GE_TWO_PI;
  while (h < 0.0f) h += GE_TWO_PI;
  STF32(base, GE_YAW_HEADING, h);
  STF32(base, GE_YAW_SMOOTH, h * 12.5f);
  STF32(base, GE_YAW_TARGET, h);
}

// No-op (kept hooked, harmless). Pitch is driven via +612 in ge_mouselook_pitch.
void ge_tick_pitch(PPCRegister& /*r11*/) {}

// THE pitch auto-level. ge_bondview_tick @0x820bcb30 eases +612 toward neutral
// when walking. When mouse-look is on, restore our pitch right after this write
// so it HOLDS where the mouse put it.
void ge_pitch_hold(PPCRegister& /*r11*/) {
  if (!ge_mouselook_on() || !g_look_bv) return;
  PPCContext* ctx; uint8_t* base; getcb(ctx, base); (void)ctx;
  uint32_t bv = LD32(base, GE_BONDVIEW_CUR);
  if (bv) STF32(base, bv + GE_BV_PITCH, g_look_pitch);
}

// GE move-to-centre: ge_bondview_control @0x820bb460 sets +524 = 1 when you move
// without pushing the look stick; the next block eases pitch back to neutral.
// Clear +524 here -- before that ease reads it -- so mouse-look pitch never
// auto-levels. Gated on FP control (intro fly-in / cutscenes keep native behavior).
void ge_no_autolevel(PPCRegister& /*r11*/) {
  if (!ge_mouselook_on()) return;
  PPCContext* ctx; uint8_t* base; getcb(ctx, base); (void)ctx;
  if (LD32(base, GE_CAMERA_MODE) != GE_CAM_FP) return;
  if (LD32(base, GE_CUTSCENE) != 0) return;
  uint32_t bv = LD32(base, GE_BONDVIEW_CUR);
  if (bv) ST32(base, bv + GE_BV_CENTER_FLAG, 0u);
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

// Runs once per controller poll, after XamInputGetState fills the slot-0 buffer
// and before the guest dispatches it. OR our keyboard buttons in, and set the
// left stick / triggers when their keys are held (pad input is preserved).
void ge_inject_keyboard(PPCRegister& /*r11*/) {
  // Attach the input listener from the controller-poll path too. InitMouseLook()
  // runs at OnCreateDialogs when Runtime::display_window() can still be null, and
  // the look hooks (ge_mouselook_*) only fire in-level -- so without this, the
  // listener never attaches while in menus and keyboard/mouse stay dead until a
  // level loads. This poll runs every frame (incl. menus), so it attaches early.
  ge_ensure_listener();
  if (!REXCVAR_GET(ge_keyboard_enable) || !ge_input_active()) return;
  PPCContext* ctx; uint8_t* base; getcb(ctx, base); (void)ctx;

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
}
