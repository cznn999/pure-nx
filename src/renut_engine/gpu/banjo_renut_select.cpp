// banjo-nx: the boot-time choice between banjo-nx's native renderer and Renut's (banjo_renut_select.h).
#include "renut_engine/gpu/banjo_renut_select.h"

#include <rex/logging.h>

#if defined(PLUME_DEKO3D)
#include "renut_engine/gpu/banjo_renut_deko3d.h"  // sa2-nx 1.0.3: MissingGameShaders
#endif

#include <string>

#include <rex/cvar.h>

// sa2-nx (2026-10-20): SA2's key. dishonored-nx (2026-10-29): Dishonored's, dishonored_renderer. pure-nx (2026-11-06):
// Pure's, pure_renderer. "xenos" keeps the
// renderer of stage 1 (ReXGlue's emulated Xenos GPU, or the PM4-ring native renderer with sa2_renderer_native); "renut"
// is Renut's Direct3D-level renderer (no PM4 ring, no EDRAM), on plume's deko3d back end on the Switch.
REXCVAR_DEFINE_STRING(pure_renderer, "xenos", "Pure",
                      "Renderer, chosen at boot. xenos = the emulated Xenos GPU (PM4 ring and EDRAM; or the native "
                      "PM4 renderer with sa2_renderer_native). renut = Renut's Direct3D-level renderer "
                      "(github.com/masterspike52/reNut, through banjo-nx and sa2-nx): the game's Direct3D draws natively "
                      "through plume (deko3d on the Switch), with no PM4 ring and no EDRAM; its own renut_* keys apply")
    .allowed({"xenos", "renut"})
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace banjo::renut_gpu {

// banjo-nx (2026-10-19, CPU proposal 22): Active() is inline (banjo_renut_select.h), a byte load after the first call.
namespace detail {
int8_t g_active = 0;
bool LatchActive() {
#if SA2_SWITCH_NO_NVK
  // sa2-nx (2026-10-20): a Switch build without the Vulkan driver (REXGLUE_SWITCH_NO_NVK) has no other renderer.
  static const bool active = [] {
#if defined(PLUME_DEKO3D)
    // sa2-nx 1.0.3 (from peggle2-nx): a build without translated game shaders (SA2_RENUT_RELEASE_SHADERS) needs the web
    // installer's deko3d_glsl.bin beside the NRO; without it the game's own pictures are not drawn.
    if (const size_t missing = deko3d::MissingGameShaders()) {
      REXLOG_ERROR("[deko3d] {}/{} game shader hashes are absent from all GLSL packages; affected draws will not "
                   "render. Check switch_shaders/deko3d_glsl.bin and the shader-generation log",
                   missing, g_shaderCacheEntryCount);
    }
#endif
    return true;
  }();
#else
  static const bool active = REXCVAR_GET(pure_renderer) == "renut";
#endif
  g_active = active ? 1 : -1;
  return active;
}
}  // namespace detail

}  // namespace banjo::renut_gpu
