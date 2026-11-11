// pure-nx (2026-11-06): what Renut's renderer (src/renut_engine, from dishonored-nx / sa2-nx) takes from sa2-nx's app
// that Pure does not have: the game frame count, the video recorder's lockstep hooks (no recorder here), and the parts
// of sa2-nx's app its enhancement code calls (sa2-nx's native Direct3D layer and its codegen self-checks): empty here. From
// dishonored-nx's src/dishonored_renut_compat.cpp and src/dishonored_me_stubs.cpp.
#include <atomic>
#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <functional>

#include "cpu/sa2_enhancements.h"
#include "me/sa2_api.h"
#include "sa2_capture.h"

std::atomic<uint64_t> g_sa2_game_frames{0};

// (g_banjo_store_cards, whether the generated code marks the store cards: src/pure_store_cards.cpp.)

namespace sa2::capture {
void Start(std::function<rex::ui::Presenter*()>) {}
void StartWithSource(std::function<bool(rex::ui::RawImage&)>) {}
bool RenutLockstepOn() { return false; }
void RenutLockstepFrame(uint64_t, const rex::ui::RawImage*) {}
void Stop() {}
void BeforeFlip() {}
}  // namespace sa2::capture

namespace sa2::api {
void Report() {}
}  // namespace sa2::api

namespace sa2::enh {
void CodegenChecksInit() {}
void CodegenChecksReport(int) {}
void FormsSelfCheck() {}
}  // namespace sa2::enh

// pure-nx (2026-11-06): defined in dishonored-nx by its native renderer (src/me) and by sa2-nx's SDK working tree
// (four-core mode, sdk/src/core/threading_switch.cpp), which Pure's SDK branch does not have.
// The relaxed vsync of sa2_renut_pacing.cpp (off by default, as dishonored_pacing_relaxed decides).
SA2_ENHANCEMENT(g_vsync_relaxed, pure_vsync_relaxed, sa2::enh::kNonCpu, false,
                "Renut's frame pacing: relaxed vsync (a late frame is shown at once)");
// No system core for guest work in Pure's build (three cores): -1, the uam compile thread stays on
// banjo_background_compile_core.
extern "C" int RexSwitchSystemCore(void) { return -1; }
extern "C" void RexSwitchSystemCoreDescribe(char* out, size_t out_len) {
  if (out && out_len) std::snprintf(out, out_len, "three cores (no system core)");
}
