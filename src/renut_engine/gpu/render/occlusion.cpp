/**
 * @file    occlusion.cpp
 * @brief   dishonored-nx (2026-10-31): real occlusion query results (occlusion.h).
 *
 * The guest's query buffer (sub_82A548C0, a batch's +812): +28 tiles, +32 bytes a report (tiles x 32), +36 reports
 * written, +40 the first report, +52 the next one's address (each event writes there, then +36 += 1, +52 += +32). A
 * report is one xe_gpu_depth_sample_counts per tile; the harvest (sub_827428B0) adds ZPass A and B of every tile,
 * little-endian as the GPU writes them, and takes report i + 1 minus report i as query i's samples. It waits while the
 * last report still holds the sentinel D3D put there (-275).
 *
 * The host's samples-passed counter only grows, as the Xenos one: report k is the counter at event k, written into
 * tile 0 (A and B halves), the other tiles zero.
 */
#include "renut_engine/gpu/render/occlusion.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#if defined(PLUME_DEKO3D)
#include <deko3d.h>
#include <plume_deko3d.h>
#endif

#include <rex/cvar.h>
#include <rex/memory/utils.h>

#include "renut_engine/gpu/backend/backend.h"
#include "renut_engine/gpu/backend/queued.h"
#include "renut_engine/gpu/backend/render_thread.h"
#include "renut_engine/gpu/backend/video.h"
#include "renut_engine/gpu/render/draw.h"      // FlushVertexFillsLocked
#include "renut_engine/gpu/guest/command_buffers.h"  // dishonored-nx (2026-11-01): RecordOcclusionReport
#include "renut_engine/gpu/render/textures.h"  // FlushTextureFillsLocked
#include "renut_engine/gpu/guest/guest_memory.h"
#include "renut_engine/gpu/log.h"

REXCVAR_DEFINE_BOOL(dishonored_renut_occlusion_queries, true, "Dishonored/Renderer",
                    "Renut (deko3d): the occlusion queries get the GPU's real sample counts, so the game skips what "
                    "walls hide. Off: every query passes (everything in view is drawn), as before 2026-10-31");
REXCVAR_DEFINE_INT32(dishonored_renut_occlusion_wait_us, 50000, "Dishonored/Renderer",
                     "dishonored_renut_occlusion_queries: how long a batch's harvest waits for the GPU's counts before "
                     "it lets every query of the batch pass (the batch's fence reads pending until the GPU has written "
                     "them, so the harvest rarely waits)")
    .range(0, 1000000);
// dishonored-nx (2026-11-01, 5 fps looking one way, 30 the other): the waiting allowed in a second while the queries
// hide much (the last measured second hid at least a quarter of them). The fixed 60 ms (kept for scenes where they
// hide little: the main menu) ran out in heavy views: 55-82 of 137 batches late, every query of them passing, so the
// GPU drew what walls hide (3,600-3,900 draws a frame, 36 ms of GPU), the next batches came later still: a spiral.
REXCVAR_DEFINE_INT32(dishonored_renut_occlusion_budget_ms, 400, "Dishonored/Renderer",
                     "dishonored_renut_occlusion_queries: the harvest's waiting allowed in a second while the queries "
                     "hide at least a quarter of what they test (60 ms otherwise)")
    .range(0, 1000);

namespace renut::gpu::occlusion {
namespace {

constexpr uint32_t kSlots = 8192;      // counter reports in flight (a batch holds 200 queries)
constexpr uint32_t kReportBytes = 16;  // deko3d: the counter, then the timestamp
constexpr uint32_t kFakeStep = 1000;   // the samples a passing query gets when there are no counts

struct Pending {
  uint64_t seq = 0;
  uint32_t tiles = 1;
};

std::mutex g_mutex;
std::unordered_map<u32, Pending> g_pending;  // by the guest report's address
std::unordered_set<u32> g_batches;            // batches harvested once (BatchPending reads only these)
uint64_t g_next_seq = 1;
uint32_t g_fake_counter = 0;

// Render thread: the block and the seq last recorded into each slot.
#if defined(PLUME_DEKO3D)
DkMemBlock g_block = nullptr;
DkGpuAddr g_gpu = 0;
#endif
std::atomic<uint8_t *> g_cpu{nullptr};
std::atomic<bool> g_flush_queued{false};
// dishonored-nx (2026-11-01): query reports recorded into command buffers (QueueReport), for the stats line.
std::atomic<uint64_t> g_recorded_reports{0};

// The reports waited for go to the GPU now: the render thread ends its open list at once (as at a game kick, its
// fills first) instead of at a kick with enough draws or at the present. One request in flight at a time.
void RequestFlush() {
  if (g_flush_queued.exchange(true, std::memory_order_acq_rel))
    return;
  render_thread::Queue([] {
    FlushVertexFillsLocked();
    FlushTextureFillsLocked();
    Video::FlushLocked();
    g_flush_queued.store(false, std::memory_order_release);
  });
}
std::array<std::atomic<uint64_t>, kSlots> g_recorded{};

// Statistics, logged every 10 s from the harvest.
struct Stats {
  uint64_t batches = 0, late = 0, missing = 0, queries = 0, hidden = 0, held = 0, flushes = 0;
  double waited_ms = 0.0, max_wait_ms = 0.0;
  std::chrono::steady_clock::time_point since = std::chrono::steady_clock::now();
};
Stats g_stats;
std::chrono::steady_clock::time_point g_budget_since{};
double g_budget_used_ms = 0.0;
// dishonored-nx (2026-11-01): the queries measured (and hidden) in the current second, and the share the last second
// with any measured hid (kept through seconds where every batch was late, so a late spiral does not shrink the budget).
uint64_t g_window_queries = 0, g_window_hidden = 0;
double g_hidden_share = 1.0;

u32 ReadWord(u32 va) {
  const auto *p = GuestPtr<const u8>(va);
  return p ? rex::memory::load_and_swap<uint32_t>(p) : 0u;
}

void WriteReport(u32 address, u32 tiles, u32 stride, u32 count) {
  const u32 lane_a = count - (count >> 1), lane_b = count >> 1;
  const u32 n = std::max<u32>(1, std::min<u32>(tiles, std::max<u32>(stride / 32, 1)));
  for (u32 t = 0; t < n && t < 16; ++t) {
    auto *counts = GuestPtr<u8>(address + t * 32);
    if (!counts)
      return;
    // Total, ZFail, ZPass, StencilFail (A, B each), as the GPU writes them (little-endian).
    const u32 words[8] = {t == 0 ? lane_a : 0u, t == 0 ? lane_b : 0u, 0, 0,
                          t == 0 ? lane_a : 0u, t == 0 ? lane_b : 0u, 0, 0};
    std::memcpy(counts, words, sizeof(words));
  }
}

void LogStatsLocked() {
  const auto now = std::chrono::steady_clock::now();
  if (now - g_stats.since < std::chrono::seconds(10))
    return;
  RGPU_INFO("[occlusion] {:.1f} s: {} batches of queries, {} with the GPU's counts, {} late and {} with reports "
            "missing (every query passing); {} queries, {} of them hidden; waited {:.1f} ms in all (most {:.2f} ms); "
            "{} fence checks held back for the GPU; {} harvests sent the open list to the GPU; {} reports recorded "
            "into command buffers (dishonored_renut_occlusion_queries)",
            std::chrono::duration<double>(now - g_stats.since).count(), g_stats.batches,
            g_stats.batches - g_stats.late - g_stats.missing, g_stats.late, g_stats.missing, g_stats.queries,
            g_stats.hidden, g_stats.waited_ms, g_stats.max_wait_ms, g_stats.held, g_stats.flushes,
            g_recorded_reports.exchange(0, std::memory_order_relaxed));
  g_stats = Stats{};
  g_stats.since = now;
}

}  // namespace

bool On() {
#if defined(PLUME_DEKO3D)
  static const bool on = IsDeko3D() && REXCVAR_GET(dishonored_renut_occlusion_queries);
  return on;
#else
  return false;  // the counter reports are deko3d's
#endif
}

void QueueReport(u32 device_va, u32 report, u32 tiles) {
  if (!report)
    return;
  uint64_t seq;
  {
    std::lock_guard lock(g_mutex);
    seq = g_next_seq++;
    g_pending[report] = {seq, std::max<u32>(tiles, 1)};
    // dishonored-nx (2026-11-01, the flickering characters and glasses): a passing count now, as before the real
    // counts. Only the batches' harvest (sub_827428B0, FillReports) puts the GPU's counts in; a query Unreal keeps on
    // its own (the bigger and the moving objects: characters, props up close) is read from this memory directly, which
    // held whatever an earlier query had left there: often a pair with no growth, so the object was hidden for a few
    // frames. A batch's reports are overwritten with the real counts at its harvest.
    g_fake_counter += kFakeStep;
    WriteReport(report, std::max<u32>(tiles, 1), 32 * std::max<u32>(tiles, 1), g_fake_counter);
    // Reports of a buffer never harvested (a batch freed with the level): dropped once their slots are reused.
    if (g_pending.size() > 4 * kSlots) {
      for (auto it = g_pending.begin(); it != g_pending.end();)
        it = it->second.seq + kSlots < g_next_seq ? g_pending.erase(it) : std::next(it);
    }
  }
  // In order with the device's draws: its Begin*Vertices draw first, as for a clear.
  FlushDeferredVertices(device_va);
  // dishonored-nx (2026-11-01): on a device recording a command buffer, an op in the buffer (its draws run when the
  // buffer runs, not now).
  if (IsRecordingCommandBuffer(device_va)) {
    FlushRecordedVertices(device_va);
    if (RecordOcclusionReport(device_va, seq)) {
      static std::atomic<bool> logged{false};
      if (!logged.exchange(true))
        RGPU_INFO("[occlusion] query events in recorded command buffers: their reports recorded in place");
      g_recorded_reports.fetch_add(1, std::memory_order_relaxed);
      return;
    }
  }
  render_thread::Queue([seq] { ReportLocked(seq); });
}

void ReportLocked(uint64_t seq) {
#if !defined(PLUME_DEKO3D)
  (void)seq;
#else
  auto &s = State();
  if (!g_block) {
    auto *device = static_cast<plume::Deko3DDevice *>(s.device.get());
    if (!device)
      return;
    const uint32_t bytes = (kSlots * kReportBytes + DK_MEMBLOCK_ALIGNMENT - 1) & ~uint32_t(DK_MEMBLOCK_ALIGNMENT - 1);
    // Uncached both ways: the game thread reads the counts as soon as the GPU has written them.
    g_block = device->makeBlock(bytes, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuUncached,
                                "occlusion reports");
    if (!g_block)
      return;
    g_gpu = dkMemBlockGetGpuAddr(g_block);
    std::memset(dkMemBlockGetCpuAddr(g_block), 0, bytes);
    g_cpu.store(static_cast<uint8_t *>(dkMemBlockGetCpuAddr(g_block)), std::memory_order_release);
    RGPU_INFO("[occlusion] the GPU's sample counts for the occlusion queries: {} reports in flight "
              "(dishonored_renut_occlusion_queries)",
              kSlots);
  }
  if (!Video::EnsureCommandListLocked() || !s.command_list)
    return;
  const uint32_t slot = uint32_t(seq % kSlots);
  auto *report = reinterpret_cast<volatile uint64_t *>(g_cpu.load(std::memory_order_relaxed) + slot * kReportBytes);
  report[0] = 0;
  report[1] = 0;
  dkCmdBufReportCounter(static_cast<plume::Deko3DCommandList *>(s.command_list)->cmd, DkCounter_SamplesPassed,
                        g_gpu + uint64_t(slot) * kReportBytes);
  g_recorded[slot].store(seq, std::memory_order_release);
#endif
}

bool BatchPending(u32 batch_va, u32 fence) {
  if (!batch_va || !fence)
    return false;
  {
    // r31 is any register value in other callers: only a batch seen at a harvest is read.
    std::lock_guard lock(g_mutex);
    if (!g_batches.count(batch_va))
      return false;
  }
  if (ReadWord(batch_va + 804) != fence)
    return false;
  const u32 query_va = ReadWord(batch_va + 812);
  if (!query_va)
    return false;
  const u32 stride = ReadWord(query_va + 32);
  const u32 written = ReadWord(query_va + 36);
  const u32 first = ReadWord(query_va + 40);
  if (!first || !written || !stride || written > 4096)
    return false;
  std::lock_guard lock(g_mutex);
  // The batch's last report: the GPU writes them in order.
  const auto it = g_pending.find(first + (written - 1) * stride);
  if (it == g_pending.end())
    return false;  // not ours: the harvest lets the batch pass
  const uint64_t seq = it->second.seq;
  // Never held back for long: past half the slots the harvest takes it (all passing if still unwritten).
  if (seq + kSlots / 2 <= g_next_seq)
    return false;
  const uint32_t slot = uint32_t(seq % kSlots);
  const uint8_t *cpu = g_cpu.load(std::memory_order_acquire);
  if (cpu && g_recorded[slot].load(std::memory_order_acquire) == seq) {
    const auto *report = reinterpret_cast<const volatile uint64_t *>(cpu + slot * kReportBytes);
    if (report[1] != 0)
      return false;
  }
  ++g_stats.held;
  RequestFlush();
  return true;
}

void FillReports(u32 batch_va, u32 query_va) {
  const u32 tiles = std::max<u32>(ReadWord(query_va + 28), 1);
  const u32 stride = ReadWord(query_va + 32);
  const u32 written = ReadWord(query_va + 36);
  const u32 first = ReadWord(query_va + 40);
  if (!first || !written || !stride || written > 4096)
    return;

  std::lock_guard lock(g_mutex);
  if (g_batches.size() < 4096)
    g_batches.insert(batch_va);
  std::vector<uint64_t> seqs(written, 0);
  bool missing = false;
  for (u32 i = 0; i < written; ++i) {
    const auto it = g_pending.find(first + i * stride);
    if (it == g_pending.end()) {
      missing = true;
      continue;
    }
    seqs[i] = it->second.seq;
    g_pending.erase(it);
  }

  // The GPU's counts, each once its slot holds this report and the GPU has written it.
  std::vector<u32> counts(written, 0);
  bool late = false;
  const auto start = std::chrono::steady_clock::now();
  if (!missing) {
    // dishonored-nx (2026-10-31): at most kWaitBudgetMs of waiting in any second. The main menu's scene waited up to
    // 4.5 s in 10 (its few queries, almost none hidden); past the budget a batch that is not ready passes at once.
    constexpr double kWaitBudgetMs = 60.0;
    if (start - g_budget_since > std::chrono::seconds(1)) {
      g_budget_since = start;
      g_budget_used_ms = 0.0;
      if (g_window_queries >= 50)
        g_hidden_share = double(g_window_hidden) / double(g_window_queries);
      g_window_queries = g_window_hidden = 0;
    }
    // dishonored-nx (2026-11-01): more while the queries pay for the wait (dishonored_renut_occlusion_budget_ms).
    static const double busy_budget_ms = double(REXCVAR_GET(dishonored_renut_occlusion_budget_ms));
    const double budget_ms = g_hidden_share >= 0.25 ? std::max(busy_budget_ms, kWaitBudgetMs) : kWaitBudgetMs;
    const bool budget_left = g_budget_used_ms < budget_ms;
    const auto deadline =
        budget_left ? start + std::chrono::microseconds(REXCVAR_GET(dishonored_renut_occlusion_wait_us)) : start;
    uint8_t *cpu = g_cpu.load(std::memory_order_acquire);
    bool flushed = false;
    for (u32 i = 0; i < written && !late; ++i) {
      const uint64_t seq = seqs[i];
      const uint32_t slot = uint32_t(seq % kSlots);
      if (seq + kSlots <= g_next_seq) {  // its slot reused since
        late = true;
        break;
      }
      for (;;) {
        if (!cpu)
          cpu = g_cpu.load(std::memory_order_acquire);
        if (cpu && g_recorded[slot].load(std::memory_order_acquire) == seq) {
          const auto *report = reinterpret_cast<const volatile uint64_t *>(cpu + slot * kReportBytes);
          if (report[1] != 0) {  // the timestamp: written with the counter
            counts[i] = u32(report[0]);
            break;
          }
        }
        if (std::chrono::steady_clock::now() >= deadline) {
          late = true;
          break;
        }
        if (!flushed) {
          RequestFlush();
          flushed = true;
          ++g_stats.flushes;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(100));
      }
    }
  }
  const double waited = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
  g_budget_used_ms += waited;

  if (missing || late) {
    // Every query passes: a count that grows by the same step at each report.
    for (u32 i = 0; i < written; ++i) {
      g_fake_counter += kFakeStep;
      WriteReport(first + i * stride, tiles, stride, g_fake_counter);
    }
  } else {
    for (u32 i = 0; i < written; ++i)
      WriteReport(first + i * stride, tiles, stride, counts[i]);
    for (u32 i = 0; i + 1 < written; ++i) {
      const bool hidden = counts[i + 1] == counts[i];
      g_stats.hidden += hidden ? 1 : 0;
      g_window_hidden += hidden ? 1 : 0;
    }
    g_window_queries += written > 1 ? written - 1 : 0;
  }
  ++g_stats.batches;
  g_stats.late += late ? 1 : 0;
  g_stats.missing += missing ? 1 : 0;
  g_stats.queries += written > 1 ? written - 1 : 0;
  g_stats.waited_ms += waited;
  g_stats.max_wait_ms = std::max(g_stats.max_wait_ms, waited);
  LogStatsLocked();
}

}  // namespace renut::gpu::occlusion
