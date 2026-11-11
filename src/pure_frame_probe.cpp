// pure-nx (2026-11-07, pure_021: races at 12-15 fps, the game thread 70-85 ms a frame): where Pure's frame goes.
//
// Pure's frame (sub_823C3350, the game state's vtable +8, once a frame from the main loop sub_823B6C10) reads the
// vertical-blank count, clamps the blanks since the last frame to 1..10 and, in its race and front-end modes, calls
// sub_823C2EC8 (state, steps) through the thunks 821DEB40 / 82215840. sub_823C2EC8 is:
//     sub_821A2260([8366F098]);          (a lock or a profiler mark: one call)
//     result = state->vtable[0x14](state, steps);   the game's update, all the 1/60 s ticks
//     state->vtable[0x18](state);                   the scene's drawing (D3D calls, Renut's capture on this thread)
//     state->vtable[0x1C](state);                   the end of the frame (the Swap)
//     return result;
// pure_frame_probe = true replaces it (on its thunk sub_821DEB40) with the same calls, timed, and times the main thread's waits in
// sub_83054C28 (the game's WaitForSingleObject: NtWaitForSingleObjectEx, 15% of the main thread in pure_021's stack
// samples) by the guest caller. Every 10 s:
//   [pure] frame probe: frames, ticks a frame, update / draw / end ms a frame (and the update per tick), the rest of
//   the main loop, and the main thread's waits by caller.
// Off: the original function. The guest stack frame is the original's (0x70 bytes, back chain and LR saved), so a
// stack walk through it reads what it read before.
#include <algorithm>
#include <atomic>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <fmt/format.h>
#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#if REX_PLATFORM_SWITCH
#include <switch.h>
#endif

REXCVAR_DEFINE_BOOL(pure_frame_probe, true, "Pure",
                    "Time Pure's frame on the main thread (update / draw / end of frame, the waits by caller) and log "
                    "it every 10 s ([pure] frame probe)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// pure-nx (2026-11-07, pure_024): the guest profile. The update costs 25-30 ms a frame whatever its time step and the
// end of the frame (the drawing) 27-38 ms, the host stack samples were flat (the guest frames do not unwind on the
// host), so this samples the GUEST stack of the main thread instead: a thread on core 2 reads the main thread's
// context every millisecond (r1 and lr, racy reads of plain memory) and walks the guest back chain (the saved LR of
// each frame is 8 bytes below its caller's stack pointer, as the Swap's stack walk reads it). Every 10 s:
//   [pure] guest profile (update|end of frame): N samples; inclusive: <return address> <share>, ...;
//   leaf: <ctx.lr> <share>, ...
// The return addresses are the guest call sites (tools: the function holding each one).
// pure-nx (2026-11-08): off by default: with elide_lr_stores ctx.lr no longer names the leaf, and the host profiler
// (switch_profile_stacks) names the functions instead.
REXCVAR_DEFINE_BOOL(pure_guest_profile, false, "Pure",
                    "Sample the main thread's guest call stack every 1 ms and log the hottest guest call sites by frame "
                    "phase every 10 s ([pure] guest profile)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace {

using Clock = std::chrono::steady_clock;

// The main thread's context and its phase for the guest profile (0 outside the frame, 1 update, 2 end of frame).
std::atomic<PPCContext *> g_main_ctx{nullptr};
std::atomic<uint8_t *> g_main_base{nullptr};
std::atomic<int> g_phase{0};
// pure-nx (2026-11-07, rex_crash: the walk read 0x701B170B, above the main thread's stack): the main thread's guest
// stack, from its PCR (r13): +0x70 the base (high address), +0x74 the end (low). Every read stays inside it.
std::atomic<uint32_t> g_stack_lo{0}, g_stack_hi{0};

// pure-nx (2026-11-08, pure_011: the race ended in a hang, the main thread blocked for good in sub_829CD948, the
// game's WaitForSingleObjectEx, every job worker idle): the main thread's current wait (the hook on sub_829CD948 below
// sets it), so the profile thread can say which wait never ends: the handle, the timeout and the guest caller (the
// LR at the call: sub_829CD948 is reached through a tail call, so that is the game function that waits).
std::atomic<uint32_t> g_wait_handle{0}, g_wait_caller{0}, g_wait_timeout{0};
std::atomic<int64_t> g_wait_since{0};  // Clock ticks since the epoch at the start of the wait; 0: not waiting

void CheckMainWait() {
  static int64_t logged_since = 0;
  static int64_t last_log = 0;
  const int64_t since = g_wait_since.load(std::memory_order_acquire);
  if (since == 0)
    return;
  const int64_t now = Clock::now().time_since_epoch().count();
  const double seconds = std::chrono::duration<double>(Clock::duration(now - since)).count();
  if (seconds < 3.0)
    return;
  const double since_log = std::chrono::duration<double>(Clock::duration(now - last_log)).count();
  if (logged_since == since && since_log < 10.0)
    return;
  logged_since = since;
  last_log = now;
  const uint32_t timeout = g_wait_timeout.load(std::memory_order_relaxed);
  REXLOG_WARN("[pure] wait watchdog: the main thread has waited {:.1f} s in sub_829CD948 on handle {:08X} (timeout "
              "{}), called from {:08X}",
              seconds, g_wait_handle.load(std::memory_order_relaxed),
              timeout == 0xFFFFFFFFu ? std::string("INFINITE") : fmt::format("{} ms", timeout),
              g_wait_caller.load(std::memory_order_relaxed));
}

inline uint32_t LoadGuest32(const uint8_t *base, uint32_t address) {
  uint32_t v;
  std::memcpy(&v, base + address, 4);
  return __builtin_bswap32(v);
}

void GuestProfileThread() {
#if REX_PLATFORM_SWITCH
  svcSetThreadCoreMask(CUR_THREAD_HANDLE, 2, 0x4);
#endif
  struct Phase {
    std::unordered_map<uint32_t, uint32_t> inclusive, leaf;
    uint32_t samples = 0;
  };
  Phase phases[3];
  auto window_start = Clock::now();
  std::vector<uint32_t> chain;
  chain.reserve(64);
  for (;;) {
#if REX_PLATFORM_SWITCH
    svcSleepThread(1'000'000);
#else
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
#endif
    CheckMainWait();  // pure-nx (2026-11-08): the wait watchdog
    PPCContext *ctx = g_main_ctx.load(std::memory_order_acquire);
    uint8_t *base = g_main_base.load(std::memory_order_acquire);
    if (!ctx || !base)
      continue;
    const int phase = g_phase.load(std::memory_order_relaxed);
    const uint32_t lr = uint32_t(reinterpret_cast<volatile uint64_t &>(ctx->lr));
    uint32_t sp = reinterpret_cast<volatile uint32_t &>(ctx->r1.u32);
    Phase &p = phases[phase];
    ++p.samples;
    if (lr >= 0x82000000u && lr < 0x84000000u)
      ++p.leaf[lr];
    chain.clear();
    if (lr >= 0x82000000u && lr < 0x84000000u)
      chain.push_back(lr);
    // Inside the main thread's stack only; a frame's back chain points up.
    const uint32_t lo = g_stack_lo.load(std::memory_order_relaxed), hi = g_stack_hi.load(std::memory_order_relaxed);
    for (int depth = 0; depth < 48; ++depth) {
      if (sp < lo || sp + 4 > hi || (sp & 3))
        break;
      const uint32_t caller_sp = LoadGuest32(base, sp);
      if (caller_sp <= sp || (caller_sp & 3) || caller_sp > hi || caller_sp - 8 < lo || caller_sp - 8 + 4 > hi)
        break;
      const uint32_t ret = LoadGuest32(base, caller_sp - 8);
      if (ret < 0x82000000u || ret >= 0x84000000u)
        break;
      if (std::find(chain.begin(), chain.end(), ret) == chain.end())
        chain.push_back(ret);
      sp = caller_sp;
    }
    for (uint32_t ret : chain)
      ++p.inclusive[ret];

    const auto now = Clock::now();
    if (now - window_start < std::chrono::seconds(10))
      continue;
    window_start = now;
    static const char *const kNames[3] = {"outside the frame", "update", "end of frame"};
    for (int i = 0; i < 3; ++i) {
      Phase &q = phases[i];
      if (q.samples < 50) {
        q = Phase{};
        continue;
      }
      auto top = [&](const std::unordered_map<uint32_t, uint32_t> &map, size_t n) {
        std::vector<std::pair<uint32_t, uint32_t>> v(map.begin(), map.end());
        std::sort(v.begin(), v.end(), [](const auto &a, const auto &b) { return a.second > b.second; });
        std::string s;
        for (size_t k = 0; k < v.size() && k < n; ++k)
          s += fmt::format(" {:08X} {:.1f}%", v[k].first, 100.0 * v[k].second / q.samples);
        return s;
      };
      REXLOG_INFO("[pure] guest profile ({}): {} samples; inclusive:{}", kNames[i], q.samples, top(q.inclusive, 60));
      REXLOG_INFO("[pure] guest profile ({}): leaf:{}", kNames[i], top(q.leaf, 40));
      q = Phase{};
    }
  }
}

void StartGuestProfile(PPCContext &ctx, uint8_t *base) {
  static bool started = false;
  if (started)
    return;
  started = true;
  if (!REXCVAR_GET(pure_guest_profile))
    return;
  const uint32_t pcr = ctx.r13.u32;
  const uint32_t stack_hi = LoadGuest32(base, pcr + 0x70), stack_lo = LoadGuest32(base, pcr + 0x74);
  if (!pcr || stack_lo >= stack_hi || stack_hi - stack_lo > 0x4000000u) {
    REXLOG_WARN("[pure] guest profile off: the main thread's stack is unknown (PCR {:08X}: {:08X}-{:08X})", pcr,
                stack_lo, stack_hi);
    return;
  }
  g_stack_lo.store(stack_lo, std::memory_order_relaxed);
  g_stack_hi.store(stack_hi, std::memory_order_relaxed);
  REXLOG_INFO("[pure] guest profile: the main thread's stack {:08X}-{:08X}", stack_lo, stack_hi);
  g_main_base.store(base, std::memory_order_release);
  g_main_ctx.store(&ctx, std::memory_order_release);
#if REX_PLATFORM_SWITCH
  // pure-nx (2026-11-07, rex_crash: std::thread is "Function not implemented" here): a libnx thread on core 2.
  static Thread thread;
  if (R_FAILED(threadCreate(
          &thread, [](void *) { GuestProfileThread(); }, nullptr, nullptr, 0x10000, 0x2E, 2)) ||
      R_FAILED(threadStart(&thread))) {
    REXLOG_WARN("[pure] guest profile: the sampling thread could not start");
    return;
  }
#else
  std::thread(GuestProfileThread).detach();
#endif
  REXLOG_INFO("[pure] guest profile on (every 1 ms, the main thread's guest stack)");
}

inline uint32_t Load32(const uint8_t *base, uint32_t address) {
  uint32_t v;
  std::memcpy(&v, base + address, 4);
  return __builtin_bswap32(v);
}

inline void Store32(uint8_t *base, uint32_t address, uint32_t value) {
  value = __builtin_bswap32(value);
  std::memcpy(base + address, &value, 4);
}

inline uint64_t Ns(Clock::time_point a, Clock::time_point b) {
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count());
}

inline void CallGuest(PPCContext &ctx, uint8_t *base, uint32_t target, uint32_t return_address) {
  ctx.lr = return_address;
  rex::runtime::ResolveIndirectFunction(target)(ctx, base);
}

// The main thread's numbers (only the main thread writes them; it logs them too).
thread_local bool t_main = false;
struct WaitSite {
  uint32_t lr = 0;
  uint32_t calls = 0;
  uint64_t ns = 0;
};
struct Totals {
  uint64_t frames = 0, ticks = 0, update = 0, draw = 0, end = 0, outside = 0, wait = 0;
  uint64_t other_frames = 0, other = 0;  // sub_823C2F50 (the other mode's frame), untimed inside
  std::array<WaitSite, 32> sites{};
  std::array<uint64_t, 11> frames_by_ticks{}, update_by_ticks{}, end_by_ticks{};  // pure_diag_steps_cycle
};
Totals g_totals;
Clock::time_point g_window_start;
Clock::time_point g_last_exit;
bool g_started = false;

void NoteWait(uint32_t lr, uint64_t ns) {
  g_totals.wait += ns;
  for (auto &site : g_totals.sites) {
    if (site.lr == lr || site.lr == 0) {
      site.lr = lr;
      ++site.calls;
      site.ns += ns;
      return;
    }
  }
}

void MaybeLog(Clock::time_point now) {
  const uint64_t window = Ns(g_window_start, now);
  if (window < 10'000'000'000ull)
    return;
  const Totals &t = g_totals;
  const double frames = double(std::max<uint64_t>(t.frames, 1));
  auto ms = [&](uint64_t ns) { return double(ns) / 1e6 / frames; };
  std::array<WaitSite, 32> sites = t.sites;
  std::sort(sites.begin(), sites.end(), [](const WaitSite &a, const WaitSite &b) { return a.ns > b.ns; });
  std::string waits;
  for (const auto &site : sites) {
    if (!site.lr || waits.size() > 400)
      break;
    waits += fmt::format(" {:08X} {:.2f} ms ({} calls);", site.lr, ms(site.ns), site.calls);
  }
  std::string by_ticks;
  for (size_t i = 1; i < t.frames_by_ticks.size(); ++i) {
    if (const uint64_t n = t.frames_by_ticks[i])
      by_ticks += fmt::format(" {} ticks: {} frames, update {:.2f} ms, end of frame {:.2f} ms;", i, n,
                              double(t.update_by_ticks[i]) / 1e6 / double(n), double(t.end_by_ticks[i]) / 1e6 / double(n));
  }
  REXLOG_INFO("[pure] frame probe by ticks:{}", by_ticks);
  REXLOG_INFO("[pure] frame probe, {:.1f} s: {} frames, {:.2f} ticks a frame; update {:.2f} ms a frame ({:.2f} ms a "
              "tick), draw {:.2f} ms, end of frame {:.2f} ms, the rest of the main loop {:.2f} ms; {} frames of the "
              "other mode (sub_823C2F50) {:.2f} ms each; the main thread waited {:.2f} ms a frame in sub_829CD948, "
              "by caller:{}",
              double(window) / 1e9, t.frames, double(t.ticks) / frames, ms(t.update),
              t.ticks ? double(t.update) / 1e6 / double(t.ticks) : 0.0, ms(t.draw), ms(t.end), ms(t.outside),
              t.other_frames, t.other_frames ? double(t.other) / 1e6 / double(t.other_frames) : 0.0, ms(t.wait),
              waits);
  g_totals = Totals{};
  g_window_start = now;
}

}  // namespace

// sub_821DEB40: the thunk (lis/ori/mtctr/bctr) sub_823C3350 calls with a direct bl for sub_823C2EC8 (state, steps),
// which this replaces (see the top of the file). pure_022: a hook on sub_823C2EC8 itself never ran (it is reached
// only through the thunk's indirect call), so the probe sits on the thunk; the generated code calls it directly once
// tools/post_codegen_pure.py has run with this file (direct_calls).
REX_EXTERN(__imp__sub_821DEB40);
REX_HOOK_RAW(sub_821DEB40) {
  static const bool on = REXCVAR_GET(pure_frame_probe);
  if (!on) {
    __imp__sub_821DEB40(ctx, base);
    return;
  }
  if (!t_main)
    REXLOG_INFO("[pure] frame probe on (build {} {})", __DATE__, __TIME__);
  t_main = true;
  StartGuestProfile(ctx, base);
  const Clock::time_point t0 = Clock::now();
  if (!g_started) {
    g_started = true;
    g_window_start = t0;
    g_last_exit = t0;
  }
  const uint64_t entry_lr = ctx.lr;
  const uint32_t state = ctx.r3.u32;
  const uint32_t steps = ctx.r4.u32;
  // The original's frame: mflr r12; stw r12,-8(r1); std r30/r31; stwu r1,-0x70(r1).
  const uint32_t sp = ctx.r1.u32;
  Store32(base, sp - 8, uint32_t(entry_lr));
  ctx.r1.u32 = sp - 0x70;
  Store32(base, sp - 0x70, sp);

  static uint32_t logged_vtable = 0;
  if (const uint32_t vtable = Load32(base, state); vtable != logged_vtable) {
    logged_vtable = vtable;
    REXLOG_INFO("[pure] frame probe: state {:08X} vtable {:08X}: update {:08X}, draw {:08X}, end of frame {:08X}",
                state, vtable, Load32(base, vtable + 0x14), Load32(base, vtable + 0x18), Load32(base, vtable + 0x1C));
  }
  ctx.r3.u32 = Load32(base, 0x8366F098u);
  CallGuest(ctx, base, 0x821A2260u, 0x823C2EF0u);
  const Clock::time_point t1 = Clock::now();
  ctx.r3.u32 = state;
  ctx.r4.u32 = steps;
  g_phase.store(1, std::memory_order_relaxed);
  CallGuest(ctx, base, Load32(base, Load32(base, state) + 0x14), 0x823C2F08u);
  const uint32_t result = ctx.r3.u32;
  const Clock::time_point t2 = Clock::now();
  ctx.r3.u32 = state;
  CallGuest(ctx, base, Load32(base, Load32(base, state) + 0x18), 0x823C2F20u);
  const Clock::time_point t3 = Clock::now();
  ctx.r3.u32 = state;
  g_phase.store(2, std::memory_order_relaxed);
  CallGuest(ctx, base, Load32(base, Load32(base, state) + 0x1C), 0x823C2F34u);
  g_phase.store(0, std::memory_order_relaxed);
  const Clock::time_point t4 = Clock::now();

  ctx.r3.u32 = result;
  ctx.r1.u32 = sp;
  ctx.lr = entry_lr;

  Totals &t = g_totals;
  ++t.frames;
  t.ticks += steps;
  t.outside += Ns(g_last_exit, t0) + Ns(t0, t1);
  t.update += Ns(t1, t2);
  t.draw += Ns(t2, t3);
  t.end += Ns(t3, t4);
  const size_t ticks = std::min<size_t>(steps, t.frames_by_ticks.size() - 1);
  ++t.frames_by_ticks[ticks];
  t.update_by_ticks[ticks] += Ns(t1, t2);
  t.end_by_ticks[ticks] += Ns(t3, t4);
  g_last_exit = t4;
  MaybeLog(t4);
}

// sub_82215840: the thunk for sub_823C2F50 (state, steps), the frame of the state's other mode (state+8 == 1): its
// update, then the drawing only when the update kept the state. Timed whole.
REX_EXTERN(__imp__sub_82215840);
REX_HOOK_RAW(sub_82215840) {
  static const bool on = REXCVAR_GET(pure_frame_probe);
  if (!on) {
    __imp__sub_82215840(ctx, base);
    return;
  }
  t_main = true;
  const Clock::time_point t0 = Clock::now();
  __imp__sub_82215840(ctx, base);
  const Clock::time_point t1 = Clock::now();
  if (g_started) {
    ++g_totals.other_frames;
    g_totals.other += Ns(t0, t1);
    MaybeLog(t1);
  }
}

// sub_829CD948: the game's WaitForSingleObjectEx (handle, timeout in ms, alertable): it calls NtWaitForSingleObjectEx
// (sub_83054C28, an import: pure_011 showed a hook there is never reached, the generated code calls the kernel
// directly). On the main thread: timed by caller, and its handle and caller kept for the wait watchdog (pure_011's
// hang). The original runs unchanged.
REX_EXTERN(__imp__sub_829CD948);
REX_HOOK_RAW(sub_829CD948) {
  if (!t_main) {
    __imp__sub_829CD948(ctx, base);
    return;
  }
  const uint32_t lr = uint32_t(ctx.lr);
  const Clock::time_point t0 = Clock::now();
  g_wait_handle.store(ctx.r3.u32, std::memory_order_relaxed);
  g_wait_timeout.store(ctx.r4.u32, std::memory_order_relaxed);
  g_wait_caller.store(lr, std::memory_order_relaxed);
  g_wait_since.store(t0.time_since_epoch().count(), std::memory_order_release);
  __imp__sub_829CD948(ctx, base);
  g_wait_since.store(0, std::memory_order_release);
  NoteWait(lr, Ns(t0, Clock::now()));
}
