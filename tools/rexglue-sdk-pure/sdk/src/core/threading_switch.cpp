/**
 * @file        rex/core/threading_switch.cpp
 * @brief       Thread shim for Horizon: affinity, priority and suspension
 *
 * See threading_switch.h for why this layer exists and for the priority
 * scheme, which is what decides whether the game stutters or not.
 */

#include <rex/platform.h>

#if REX_PLATFORM_SWITCH

#include "threading_switch.h"

/*
 * switch.h is included here, privately. This file uses none of the rex::thread
 * types, so libnx polluting the global namespace (Thread, Timer, Event,
 * Handle...) does no harm. The header above does not include it, precisely so
 * that threading_posix.cpp does not suffer from it.
 */
#include <switch.h>

#include <errno.h>
#include <malloc.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>

#include <atomic>   // pure-nx (2026-11-08): the system core (renut-nx's four-core mode)
#include <cstdio>   // pure-nx (2026-11-08): std::snprintf, std::fopen
#include <cstdlib>  // pure-nx (2026-11-08): std::strtoull
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>

#include <rex/cvar.h>  // pure-nx (2026-11-08): pure_system_core
#include <sys/stat.h>  // pure-nx (2026-11-08): mkdir (RexSwitchEnsureForwarderNpdm)

// pure-nx (2026-11-08, the user's request after pure_011: races at 13-16 fps, the main thread waiting for the job
// workers that share cores 1-2 with the FMOD mixer, the XMA Decoder and the render thread): renut-nx's four-core mode
// (sdk/src/core/threading_switch.cpp and docs/four-core-mode.md there; from NetherSX2_nx's, MIT, NaGaa95). A process
// whose NPDM grants CPU 0-3 gets core 3 too; the audio threads go there alone and the game's threads stay on 0-2.
REXCVAR_DEFINE_INT32(pure_system_core, 1, "Pure/Threads",
                     "When the process is granted all four cores (a Sphaira forwarder of this NRO, whose NPDM Pure "
                     "writes): 0 = leave core 3 to the system, 1 = the audio threads (the audio output, the Audio "
                     "Worker, the game's FMOD threads) on core 3 alone, every other thread on cores 0-2, 2 = as 1 and "
                     "the other threads allowed on core 3 too. No effect with the usual three cores")
    .range(0, 2)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_STRING(pure_forwarder_program_id, "", "Pure/Threads",
                      "Program id (16 hex digits) of a forwarder that launches Pure, besides the ids Sphaira's "
                      "\"Install Forwarder\" gives this NRO (found from its path). Running under one of them, Pure "
                      "writes sdmc:/atmosphere/contents/<id>/exefs/main.npdm (an NPDM granting CPU 0-3, Atmosphere's "
                      "ExeFS file replacement) when it is missing or different; the next launch gets all four cores")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace {

/*
 * Registry pthread_t -> Horizon Handle.
 *
 * libnx exposes no way to get the handle of another thread, and the SDK
 * changes the priority and affinity of other threads, so it has to be tracked
 * by hand.
 *
 * Not a hot path: it is touched when threads are created and destroyed, never
 * per frame. A plain mutex is more than enough.
 */
struct Register {
  std::mutex m;
  std::unordered_map<uintptr_t, RexSwitchHandle> map;
};

Register& register_value() {
  static Register r;
  return r;
}

}  // namespace

extern "C" {
/* libnx: the heap size asked for when there is no loader heap override (runtime_switch.cpp sets it). */
extern size_t __nx_heap_size;
}

extern "C" {

void RexSwitchRegisterCurrentThread(void) {
  auto& r = register_value();
  std::lock_guard<std::mutex> lock(r.m);
  r.map[reinterpret_cast<uintptr_t>(pthread_self())] = threadGetCurHandle();
}

void RexSwitchUnregisterCurrentThread(void) {
  auto& r = register_value();
  std::lock_guard<std::mutex> lock(r.m);
  r.map.erase(reinterpret_cast<uintptr_t>(pthread_self()));
}

RexSwitchHandle RexSwitchHandleFor(pthread_t thread) {
  if (pthread_equal(thread, pthread_self()))
    return threadGetCurHandle();

  auto& r = register_value();
  std::lock_guard<std::mutex> lock(r.m);
  auto it = r.map.find(reinterpret_cast<uintptr_t>(thread));
  return it == r.map.end() ? REX_SWITCH_INVALID_HANDLE : it->second;
}

/* --- affinity ---------------------------------------------------------- */

int pthread_getaffinity_np(pthread_t thread, size_t size, cpu_set_t* set) {
  if (set == nullptr || size < sizeof(cpu_set_t))
    return EINVAL;

  RexSwitchHandle h = RexSwitchHandleFor(thread);
  if (h == REX_SWITCH_INVALID_HANDLE)
    return ESRCH;

  s32 preferred = 0;
  u64 mask = 0;
  if (R_FAILED(svcGetThreadCoreMask(&preferred, &mask, h)))
    return EINVAL;

  set->bits = mask;
  return 0;
}

int pthread_setaffinity_np(pthread_t thread, size_t size, const cpu_set_t* set) {
  if (set == nullptr || size < sizeof(cpu_set_t))
    return EINVAL;

  RexSwitchHandle h = RexSwitchHandleFor(thread);
  if (h == REX_SWITCH_INVALID_HANDLE)
    return ESRCH;

  /*
   * Horizon also wants a preferred core. The lowest one in the mask is
   * taken; -1 would mean "whatever it already had", which is not what the
   * POSIX API being imitated asks for.
   */
  s32 preferred = -1;
  for (int i = 0; i < 4; i++) {
    if (set->bits & (1ull << i)) { preferred = i; break; }
  }
  if (preferred < 0)
    return EINVAL;  /* empty mask: POSIX does not allow it either */

  /*
   * Only 4 cores exist. Asking for more is not a caller error (the SDK
   * passes 64-bit masks), so it is trimmed silently.
   */
  const u32 mask = static_cast<u32>(set->bits & 0xF);

  if (R_FAILED(svcSetThreadCoreMask(h, preferred, mask)))
    return EPERM;
  return 0;
}

/*
 * --- priority ------------------------------------------------------------
 *
 * sched_param::sched_priority is passed straight through as the Horizon
 * priority, without inverting the scale. This is deliberate: callers using the
 * REX_SWITCH_PRIO_* constants expect Horizon numbers, where lower means higher
 * priority. Inverting it here to look like POSIX would only turn the priority
 * scheme upside down, which is exactly the bug that causes stutter and audio
 * dropouts.
 */

int pthread_getschedparam(pthread_t thread, int* policy, struct sched_param* param) {
  RexSwitchHandle h = RexSwitchHandleFor(thread);
  if (h == REX_SWITCH_INVALID_HANDLE)
    return ESRCH;

  s32 prio = 0;
  if (R_FAILED(svcGetThreadPriority(&prio, h)))
    return EINVAL;

  if (policy) *policy = SCHED_FIFO;
  if (param) param->sched_priority = prio;
  return 0;
}

int pthread_setschedparam(pthread_t thread, int policy, const struct sched_param* param) {
  (void)policy;
  if (param == nullptr)
    return EINVAL;

  RexSwitchHandle h = RexSwitchHandleFor(thread);
  if (h == REX_SWITCH_INVALID_HANDLE)
    return ESRCH;

  if (R_FAILED(svcSetThreadPriority(h, static_cast<u32>(param->sched_priority))))
    return EPERM;
  return 0;
}

int pthread_attr_setschedpolicy(pthread_attr_t* attr, int policy) {
  (void)attr;
  (void)policy;
  /* Horizon has a single policy. Accepting and doing nothing is correct. */
  return 0;
}

/* --- robust mutexes: they do not exist --------------------------------- */

int pthread_mutexattr_setrobust(pthread_mutexattr_t* attr, int robustness) {
  (void)attr;
  /*
   * PTHREAD_MUTEX_STALLED is accepted (the real behavior), and ROBUST is
   * accepted without providing it: the SDK uses it as a safeguard against a
   * thread dying while holding the mutex, which should not happen in the guest.
   */
  if (robustness != PTHREAD_MUTEX_STALLED && robustness != PTHREAD_MUTEX_ROBUST)
    return EINVAL;
  return 0;
}

int pthread_mutex_consistent(pthread_mutex_t* mutex) {
  (void)mutex;
  return 0;
}

int pthread_mutexattr_getrobust(const pthread_mutexattr_t* attr, int* robustness) {
  (void)attr;
  if (robustness) *robustness = PTHREAD_MUTEX_STALLED;
  return 0;
}

/*
 * --- suspension ----------------------------------------------------------
 *
 * The SDK suspends threads by sending them a signal. Horizon does not deliver
 * signals, so that cannot work however much shimming is added: the system
 * mechanism has to be used.
 *
 * A difference in semantics worth knowing: svcSetThreadActivity stops the
 * thread wherever it is, not at a safe point chosen by the thread itself. For
 * what the SDK does with this (suspending guest threads, which is what the
 * original console did) that is correct. For any use that needs the thread to
 * stop at a specific place, it is not.
 */

int RexSwitchPauseThread(pthread_t thread) {
  RexSwitchHandle h = RexSwitchHandleFor(thread);
  if (h == REX_SWITCH_INVALID_HANDLE)
    return ESRCH;
  if (pthread_equal(thread, pthread_self()))
    return EDEADLK;  /* pausing oneself cannot be undone */
  return R_SUCCEEDED(svcSetThreadActivity(h, ThreadActivity_Paused)) ? 0 : EPERM;
}

int RexSwitchResumeThread(pthread_t thread) {
  RexSwitchHandle h = RexSwitchHandleFor(thread);
  if (h == REX_SWITCH_INVALID_HANDLE)
    return ESRCH;
  return R_SUCCEEDED(svcSetThreadActivity(h, ThreadActivity_Runnable)) ? 0 : EPERM;
}

/* --- priority of the current thread ------------------------------------ */

uint32_t RexSwitchCurrentThreadId(void) {
  /* The current thread's handle serves as a unique id, stable while it lives. */
  return static_cast<uint32_t>(threadGetCurHandle());
}

/*
 * threadGetSelf() only returns something for threads that libnx created with threadCreate. Ours
 * come from pthread_create (rex::thread::Thread::Create -> PosixThread), so if the TLS does not carry
 * libnx's ThreadVars, this used to be a silent no-op: the priority was not applied and the log said
 * it was, because it only printed the cvar value.
 *
 * CUR_THREAD_HANDLE (0xFFFF8000) is the pseudo-handle that the Horizon kernel always understands as
 * "the calling thread", wherever it came from. The result is returned so that it can be checked in
 * the log instead of assumed.
 */
bool RexSwitchSetCurrentThreadPriorityOk(int priority) {
  Thread* yo = threadGetSelf();
  Handle h = (yo != nullptr) ? yo->handle : CUR_THREAD_HANDLE;
  if (R_SUCCEEDED(svcSetThreadPriority(h, static_cast<u32>(priority)))) {
    return true;
  }
  if (h != CUR_THREAD_HANDLE) {
    return R_SUCCEEDED(svcSetThreadPriority(CUR_THREAD_HANDLE, static_cast<u32>(priority)));
  }
  return false;
}

void RexSwitchSetCurrentThreadPriority(int priority) {
  (void)RexSwitchSetCurrentThreadPriorityOk(priority);
}

/*
 * Preferred core of the current thread.
 *
 * All our threads run with "preferred core -1", which on Horizon means the process default core:
 * the same one for all of them. With the ring at 94.7 % of a core and the game's main thread at
 * 88.2 %, both prefer the same place and the kernel has to keep moving them. Each migration throws
 * away the thread's L1 and part of its L2, and that does not show in the mean: it shows in the
 * variance, which is exactly what the player notices.
 *
 * The mask is not touched. The thread's existing mask is passed, so this only says "start here",
 * not "you may only run here": if the core is busy, the kernel can still move the thread. That is
 * why it cannot cause starvation, which is what makes real affinity pinning dangerous.
 *
 * Returns false if the kernel does not accept it (for example if the core is not in the process
 * mask: that is the "Too few processor cores" error seen when trying to use the fourth core).
 */
bool RexSwitchSetCurrentThreadCore(int core) {
  Thread* yo = threadGetSelf();
  Handle h = (yo != nullptr) ? yo->handle : CUR_THREAD_HANDLE;
  s32 preferred = 0;
  u64 mask = 0;
  if (R_FAILED(svcGetThreadCoreMask(&preferred, &mask, h))) {
    if (h == CUR_THREAD_HANDLE ||
        R_FAILED(svcGetThreadCoreMask(&preferred, &mask, CUR_THREAD_HANDLE))) {
      return false;
    }
    h = CUR_THREAD_HANDLE;
  }
  if (core >= 0 && (mask & (u64(1) << core)) == 0) {
    return false;  // that core is not this process's: do not force it
  }
  return R_SUCCEEDED(svcSetThreadCoreMask(h, core, mask));
}

bool RexSwitchSetThreadMaskOf(void* native_handle, int preferred, unsigned long long mask) {
  pthread_t thread;
  static_assert(sizeof(thread) == sizeof(native_handle));
  std::memcpy(&thread, &native_handle, sizeof(thread));
  const RexSwitchHandle h = RexSwitchHandleFor(thread);
  const u64 m = u64(mask) & 0xF;  // pure-nx (2026-11-08): core 3 too when the process has it (the kernel decides)
  if (h == REX_SWITCH_INVALID_HANDLE || m == 0 || preferred < 0 || !(m & (u64(1) << preferred))) return false;
  return R_SUCCEEDED(svcSetThreadCoreMask(h, preferred, m));
}

bool RexSwitchSetCurrentThreadMask(int preferred, unsigned long long mask) {
  Thread* yo = threadGetSelf();
  Handle h = (yo != nullptr) ? yo->handle : CUR_THREAD_HANDLE;
  s32 current_preferred = 0;
  u64 current = 0;
  if (R_FAILED(svcGetThreadCoreMask(&current_preferred, &current, h))) {
    if (h == CUR_THREAD_HANDLE ||
        R_FAILED(svcGetThreadCoreMask(&current_preferred, &current, CUR_THREAD_HANDLE))) {
      return false;
    }
    h = CUR_THREAD_HANDLE;
  }
  // pure-nx (2026-11-08): the process's cores, core 3 included when granted (the kernel refuses a core it lacks).
  const u64 m = u64(mask) & 0xF;
  if (m == 0 || preferred < 0 || !(m & (u64(1) << preferred))) {
    return false;
  }
  return R_SUCCEEDED(svcSetThreadCoreMask(h, preferred, m));
}

/*
 * The real pin, with the mask reduced to a single core.
 *
 * Measured: a preferred core is not enough. Horizon accepts it and moves the thread anyway (0.24
 * migrations per loop, wherever it is set). This takes the other cores away from the thread.
 *
 * Different from RexSwitchSetCurrentThreadCore and more dangerous: if that core saturates, the
 * thread does not get to run. That is why it sits behind a cvar that is off by default and is
 * checked in the log. See docs/platform-notes.md (Threads).
 */
bool RexSwitchPinCurrentThreadToCore(int core) {
  if (core < 0) {
    return false;
  }
  Thread* yo = threadGetSelf();
  Handle h = (yo != nullptr) ? yo->handle : CUR_THREAD_HANDLE;
  s32 preferred = 0;
  u64 mask = 0;
  if (R_FAILED(svcGetThreadCoreMask(&preferred, &mask, h))) {
    if (h == CUR_THREAD_HANDLE ||
        R_FAILED(svcGetThreadCoreMask(&preferred, &mask, CUR_THREAD_HANDLE))) {
      return false;
    }
    h = CUR_THREAD_HANDLE;
  }
  const u64 that_only = u64(1) << core;
  if ((mask & that_only) == 0) {
    return false;  // that core is not this process's
  }
  return R_SUCCEEDED(svcSetThreadCoreMask(h, core, that_only));
}

/*
 * Which core the calling thread is running on right now. This is what allows counting migrations:
 * it is read on every loop iteration and counted each time it changes. There is no syscall to ask
 * it about another thread, so each thread has to look at itself.
 */
int RexSwitchCurrentCore(void) { return int(svcGetCurrentProcessorNumber()); }

void RexSwitchDescribeThreadFailure(size_t stack_size, char* out, size_t out_len) {
  if (out == nullptr || out_len == 0) {
    return;
  }
  // 1. libnx allocates stack + TLS + reent as one block with memalign(0x1000, ...); 64 KB covers TLS and reent.
  const size_t block = ((stack_size + 0xFFF) & ~size_t(0xFFF)) + 0x10000;
  void* probe = memalign(0x1000, block);
  const bool heap_ok = probe != nullptr;
  free(probe);
  const struct mallinfo mi = mallinfo();
  // With a loader (hbloader) the heap is the loader's, whatever __nx_heap_size says.
  const size_t heap_total = envHasHeapOverride() ? size_t(envGetHeapOverrideSize()) : __nx_heap_size;
  // 2. Room for the stack's mirror in the stack region (then svcMapMemory).
  virtmemLock();
  void* const mirror = virtmemFindStack(block, 0x4000);
  virtmemUnlock();
  // 3. A kernel thread slot (svcCreateThread).
  s64 threads_now = -1;
  s64 threads_cap = -1;
  u64 limit = 0;
  if (R_SUCCEEDED(svcGetInfo(&limit, InfoType_ResourceLimit, CUR_PROCESS_HANDLE, 0)) && limit != 0) {
    svcGetResourceLimitCurrentValue(&threads_now, Handle(limit), LimitableResource_Threads);
    svcGetResourceLimitLimitValue(&threads_cap, Handle(limit), LimitableResource_Threads);
    svcCloseHandle(Handle(limit));
  }
  snprintf(out, out_len,
           "heap %s a %zu KB block (heap %zu MB, arena %zu MB, in use %zu MB, free %zu MB in %zu chunks); "
           "stack region %s; kernel threads %lld of %lld",
           heap_ok ? "has" : "has NO", block >> 10, heap_total >> 20, size_t(mi.arena) >> 20,
           size_t(mi.uordblks) >> 20, size_t(mi.fordblks) >> 20, size_t(mi.ordblks),
           mirror != nullptr ? "has room" : "is FULL", (long long)threads_now, (long long)threads_cap);
}

/*
 * --- the system core (pure-nx, 2026-11-08; renut-nx's port of NetherSX2_nx's 4-core mode, MIT, NaGaa95) ------------
 *
 * With four cores granted the top one is kept for background threads, each moved there alone; the game's threads
 * keep the first three. libnx's pthread_create gives every new thread the whole grant (newlib.o,
 * __syscall_thread_create: svcSetThreadCoreMask(handle, -1, InfoType_CoreMask)), so __wrap_svcSetThreadCoreMask
 * (--wrap, sdk/cmake/rexglue_switch.cmake) takes core 3 out of every mask of more than one core, unless
 * pure_system_core is 2. A request for core 3 alone goes through. With three cores nothing changes.
 */
}  // extern "C"

namespace {
std::atomic<int> g_sys_state{0};  // 0: not read yet, 1: read
std::atomic<unsigned> g_process_mask{0x7};
std::atomic<int> g_system_core{-1};     // 3 when granted and pure_system_core >= 1
std::atomic<int> g_system_mode{0};
std::atomic<unsigned> g_strip_mask{0};  // bits taken out of multi-core masks
std::mutex g_sys_mutex;

void ReadSystemCore(bool with_cvar) {
  u64 mask = 0;
  if (R_FAILED(svcGetInfo(&mask, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0)) || mask == 0) {
    mask = 0x7;  // an application's usual three cores
  }
  mask &= 0xF;
  const int top = __builtin_popcountll(mask) >= 4 ? 63 - __builtin_clzll(mask) : -1;
  // Before the cvars are parsed (a thread made before main): the default layout.
  const int mode = top < 0 ? 0 : (with_cvar ? int(REXCVAR_GET(pure_system_core)) : 1);
  g_process_mask.store(unsigned(mask), std::memory_order_relaxed);
  g_system_mode.store(mode, std::memory_order_relaxed);
  g_system_core.store(mode >= 1 ? top : -1, std::memory_order_relaxed);
  g_strip_mask.store((top >= 0 && mode <= 1) ? (1u << top) : 0u, std::memory_order_relaxed);
  g_sys_state.store(1, std::memory_order_release);
}

inline void EnsureSystemCore() {
  if (g_sys_state.load(std::memory_order_acquire) == 0) {
    std::lock_guard<std::mutex> lock(g_sys_mutex);
    if (g_sys_state.load(std::memory_order_relaxed) == 0) ReadSystemCore(false);
  }
}

#include "forwarder_npdm.inc"  // kForwarderNpdm (renut-nx tools/forwarder: CPU 0-3, 39-bit address space)
std::string g_npdm_status = "not checked";
}  // namespace

extern "C" {

Result __real_svcSetThreadCoreMask(Handle handle, s32 preferred_core, u32 affinity_mask);
Result __wrap_svcSetThreadCoreMask(Handle handle, s32 preferred_core, u32 affinity_mask) {
  EnsureSystemCore();
  const u32 strip = g_strip_mask.load(std::memory_order_relaxed);
  // A mask of more than one core loses the system core (an app core stays); a request for the system core alone
  // (the audio threads) goes through.
  if (strip != 0 && (affinity_mask & strip) != 0 && (affinity_mask & ~strip) != 0) {
    affinity_mask &= ~strip;
    if (preferred_core >= 0 && (strip & (1u << preferred_core)) != 0) preferred_core = -1;
  }
  return __real_svcSetThreadCoreMask(handle, preferred_core, affinity_mask);
}

int RexSwitchInitSystemCore(void) {
  {
    std::lock_guard<std::mutex> lock(g_sys_mutex);
    ReadSystemCore(true);
  }
  // The main thread: a forwarder may hand it over with the whole grant.
  const u32 strip = g_strip_mask.load(std::memory_order_relaxed);
  if (strip != 0) {
    s32 preferred = 0;
    u64 mask = 0;
    if (R_SUCCEEDED(svcGetThreadCoreMask(&preferred, &mask, CUR_THREAD_HANDLE)) && (mask & strip) != 0 &&
        (mask & ~u64(strip)) != 0) {
      svcSetThreadCoreMask(CUR_THREAD_HANDLE, -1, u32(mask));  // (the wrap takes the system core out)
    }
  }
  return g_system_core.load(std::memory_order_relaxed);
}

unsigned RexSwitchProcessCoreMask(void) {
  EnsureSystemCore();
  return g_process_mask.load(std::memory_order_relaxed);
}

int RexSwitchSystemCore(void) {
  EnsureSystemCore();
  return g_system_core.load(std::memory_order_relaxed);
}

int RexSwitchSystemCoreMode(void) {
  EnsureSystemCore();
  return g_system_mode.load(std::memory_order_relaxed);
}

unsigned RexSwitchAppCoreMask(void) {
  EnsureSystemCore();
  return g_process_mask.load(std::memory_order_relaxed) & ~g_strip_mask.load(std::memory_order_relaxed);
}

// pure-nx (2026-11-08): the calling thread on 'mask' as given, the system core included (the --wrap strips it from any
// multi-core mask in mode 1): the game's job workers on cores 1-3. Cores the process lacks are dropped.
bool RexSwitchSetCurrentThreadMaskAny(int preferred, unsigned mask) {
  const u32 m = u32(mask) & RexSwitchProcessCoreMask();
  if (m == 0) return false;
  if (preferred >= 0 && !(m & (1u << preferred))) preferred = -1;
  return R_SUCCEEDED(__real_svcSetThreadCoreMask(CUR_THREAD_HANDLE, preferred, m));
}

bool RexSwitchMoveCurrentThreadToSystemCore(int priority) {
  const int core = RexSwitchSystemCore();
  if (core < 0) return false;
  if (R_FAILED(svcSetThreadCoreMask(CUR_THREAD_HANDLE, core, 1u << core))) return false;
  if (priority >= 0) (void)RexSwitchSetCurrentThreadPriorityOk(priority);
  return true;
}

// Whether 'program' is an id Sphaira's "Install Forwarder" gives a forwarder of this NRO (renut-nx 1.0.0; sphaira
// source/owo.cpp): the first 8 bytes of the SHA-256 of the NRO path written twice, as 05xxxxxxxxxxx000, or, in its
// native-game mode, 01xxxxxxxxxxx000 with bit 12 clear; the path with "sdmc:" and without. A retail game's id (title
// takeover) never matches, so its NPDM is never replaced.
extern int __system_argc;  // libnx: the NRO's path in argv[0]
extern char** __system_argv;
static bool SphairaForwarderOfThisNro(u64 program) {
  if (__system_argc <= 0 || !__system_argv || !__system_argv[0] || !__system_argv[0][0]) return false;
  const std::string path = __system_argv[0];
  const std::string rooted = path.rfind("sdmc:", 0) == 0 ? path.substr(5) : path;
  for (const std::string& p : {std::string("sdmc:") + rooted, rooted}) {
    const std::string hashed = p + p;
    u64 digest[4] = {};
    sha256CalculateHash(digest, hashed.data(), hashed.size());
    const u64 h = digest[0];
    if (program == (0x0500000000000000ull | (h & 0x00FFFFFFFFFFF000ull)) ||
        program == (0x0100000000000000ull | (h & 0x00FFFFFFFFFFE000ull)))
      return true;
  }
  return false;
}

int RexSwitchEnsureForwarderNpdm(void) {
  const std::string want_text = REXCVAR_GET(pure_forwarder_program_id);
  const u64 want = want_text.empty() ? 0 : std::strtoull(want_text.c_str(), nullptr, 16);
  u64 program = 0;
  // pure-nx (2026-11-09, release): pure_forwarder_program_id only names a homebrew forwarder's id (05xxxxxxxxxxx000),
  // never a retail title's: run by title takeover with a game's id there, the NPDM would replace that game's own.
  const bool want_ok = want != 0 && (want >> 56) == 0x05 && (want & 0xFFF) == 0;
  if (R_FAILED(svcGetInfo(&program, InfoType_ProgramId, CUR_PROCESS_HANDLE, 0)) ||
      ((!want_ok || program != want) && !SphairaForwarderOfThisNro(program))) {
    char text[200];
    std::snprintf(text, sizeof(text),
                  "not written: running as %016llX, not a Sphaira forwarder of %s (nor pure_forwarder_program_id)",
                  (unsigned long long)program, (__system_argc > 0 && __system_argv && __system_argv[0])
                                                   ? __system_argv[0] : "?");
    g_npdm_status = text;
    return 0;
  }
  // The NPDM with this program id in ACI0 and in ACID's range, and Pure's name.
  unsigned char npdm[sizeof(kForwarderNpdm)];
  std::memcpy(npdm, kForwarderNpdm, sizeof(npdm));
  std::memset(npdm + 0x20, 0, 0x10);
  std::memcpy(npdm + 0x20, "pure-nx", 7);
  std::memcpy(npdm + kForwarderNpdmAci0 + 0x10, &program, 8);
  std::memcpy(npdm + kForwarderNpdmAcid + 0x210, &program, 8);
  std::memcpy(npdm + kForwarderNpdmAcid + 0x218, &program, 8);
  char dir[96], path[128];
  std::snprintf(dir, sizeof(dir), "sdmc:/atmosphere/contents/%016llX/exefs", (unsigned long long)program);
  std::snprintf(path, sizeof(path), "%s/main.npdm", dir);
  if (FILE* f = std::fopen(path, "rb")) {
    unsigned char have[sizeof(npdm) + 1];
    const size_t n = std::fread(have, 1, sizeof(have), f);
    std::fclose(f);
    if (n == sizeof(npdm) && std::memcmp(have, npdm, sizeof(npdm)) == 0) {
      g_npdm_status = std::string("in place: ") + path;
      return 1;
    }
  }
  char part[96];
  for (const char* sub : {"sdmc:/atmosphere", "sdmc:/atmosphere/contents"}) mkdir(sub, 0777);
  std::snprintf(part, sizeof(part), "sdmc:/atmosphere/contents/%016llX", (unsigned long long)program);
  mkdir(part, 0777);
  mkdir(dir, 0777);
  FILE* f = std::fopen(path, "wb");
  const bool ok = f && std::fwrite(npdm, 1, sizeof(npdm), f) == sizeof(npdm);
  if (f) std::fclose(f);
  g_npdm_status =
      std::string(ok ? "written (launch the forwarder again for four cores): " : "could NOT be written: ") + path;
  return ok ? 2 : -1;
}

void RexSwitchSystemCoreDescribe(char* out, size_t out_len) {
  if (!out || !out_len) return;
  std::snprintf(out, out_len,
                "process cores 0x%X, system core %d (pure_system_core %d; game threads on 0x%X); forwarder NPDM %s",
                RexSwitchProcessCoreMask(), RexSwitchSystemCore(), RexSwitchSystemCoreMode(), RexSwitchAppCoreMask(),
                g_npdm_status.c_str());
}

}  // extern "C"

#endif  /* REX_PLATFORM_SWITCH */
