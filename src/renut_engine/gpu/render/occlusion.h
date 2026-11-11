/**
 * @file    occlusion.h
 * @brief   dishonored-nx (2026-10-31): real occlusion query results for UE3's batched queries.
 *
 * Dishonored (UE3) culls hidden objects with hardware occlusion queries: an event per query writes the GPU's
 * passed-sample count into a report (sub_82A549E0), a batch of up to 200 queries is harvested once its fence is done
 * (sub_827428B0: end minus begin per query). Renut wrote a growing fake count at each event, so every query passed and
 * the game drew everything in the view frustum, walls or not (thousands of draws a frame looking across a level).
 *
 * Here each event becomes a samples-passed counter report on the host GPU (deko3d: dkCmdBufReportCounter) in its place
 * among the draws, and the harvest copies the GPU's counts into the guest's reports first. A report the GPU has not
 * written in time makes the batch's queries all pass, as before.
 */
#pragma once

#include <rex/types.h>

namespace renut::gpu::occlusion {

// The real counts are on (dishonored_renut_occlusion_queries and the deko3d back end).
bool On();

// Game thread, at the query event: the report the event writes ('report', 'tiles' 32-byte tiles) gets the counter
// from the GPU, queued among the device's draws.
void QueueReport(u32 device_va, u32 report, u32 tiles);

// Game thread, before the batch's harvest: the reports of the query buffer at 'query_va' (the batch's +812) are
// written into guest memory, real counts or (late or missing) every query passing.
void FillReports(u32 batch_va, u32 query_va);

// Game thread, in IsFencePending: true for the fence of a batch whose last report the GPU has not written yet
// (sub_82742AB0 checks a batch's fence before it harvests it, the batch in r31). The game then takes another batch,
// or makes one, as when the console's GPU is behind, instead of the harvest waiting.
bool BatchPending(u32 batch_va, u32 fence);

// Render thread, State().mutex held: the counter report for 'seq' into the open command list.
void ReportLocked(uint64_t seq);

}  // namespace renut::gpu::occlusion
