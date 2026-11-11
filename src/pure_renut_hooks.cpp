// pure-nx (2026-11-06): Pure's own hooks around Renut's renderer (src/renut_engine): the shader dump.
//
// Pure's shader objects are set up by Direct3D's XGRegisterVertexShader (0x829DD270) and XGRegisterPixelShader
// (0x829DCF98), the same code as dishonored-nx's sub_82A48688 / sub_82A48490 (tools/census): a vertex shader object
// (type 6) keeps its container's header and constant tables at +0x368 and the microcode pointer at +0x20, a pixel
// shader object (type 7) at +0x28 and +0x18. These hooks hand over to Renut's (renut_engine/gpu/d3d/hooks/shader.cpp,
// renut_native_sub_X) and, with pure_dump_shaders = true, write every container to shaders/ beside the executable as
// vs_<hash>.bin / ps_<hash>.bin: the one-container-a-file layout renut_shaders/ (the Switch shader package's input)
// has. From src/dishonored_me_hooks.cpp of dishonored-nx.

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <system_error>
#include <unordered_set>
#include <vector>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#define XXH_INLINE_ALL
#include <xxhash.h>

#include "renut_engine/gpu/banjo_renut_select.h"
#include "renut_engine/gpu/banjo_shared_hooks.h"

REXCVAR_DEFINE_BOOL(pure_dump_shaders, false, "Pure",
                    "Write every shader the game creates to shaders/ next to the executable (vs_/ps_<hash>.bin)");

namespace {

inline uint32_t Load32(const uint8_t* base, uint32_t address) {
  uint32_t v;
  std::memcpy(&v, base + address, 4);
  return __builtin_bswap32(v);
}

std::mutex g_dump_mutex;
std::unordered_set<uint64_t> g_dumped_hashes;

// The container a D3D shader object carries: its header and constant tables at object+0x368 (VS) / +0x28 (PS) with
// the virtual size at +4 and the physical (microcode) size at +8, and the microcode at the pointer object+0x20 (VS) /
// +0x18 (PS), in a physical-memory view. Written as one blob (header part, then microcode): shaders/vs_<hash>.bin /
// ps_<hash>.bin.
void DumpShaderObject(const uint8_t* base, uint32_t object, bool vertex) {
  if (!REXCVAR_GET(pure_dump_shaders) || object < 0x40000000 || object >= 0x7F000000) return;
  if ((Load32(base, object) & 0xFF) != (vertex ? 6u : 7u)) return;
  const uint32_t header = object + (vertex ? 0x368 : 0x28);
  const uint32_t virtual_size = Load32(base, header + 4);
  const uint32_t physical_size = Load32(base, header + 8);
  const uint32_t microcode = Load32(base, object + (vertex ? 0x20 : 0x18));
  if ((Load32(base, header) & 0xFFFFFF00u) != 0x102A1100u || virtual_size < 28 || virtual_size > 0x40000 ||
      !physical_size || physical_size > 0x40000 || microcode < 0xA0000000u) {
    REXLOG_WARN("[shaders] {} object {:08X} has no container (word {:08X}, sizes {}/{}, microcode {:08X})",
                vertex ? "VS" : "PS", object, Load32(base, header), virtual_size, physical_size, microcode);
    return;
  }
  std::vector<uint8_t> blob(size_t(virtual_size) + physical_size);
  std::memcpy(blob.data(), base + header, virtual_size);
  std::memcpy(blob.data() + virtual_size,
              rex::system::kernel_state()->memory()->TranslateVirtual<const uint8_t*>(microcode), physical_size);
  const uint64_t hash = XXH3_64bits(blob.data(), blob.size());
  {
    std::lock_guard lock(g_dump_mutex);
    if (!g_dumped_hashes.insert(hash).second) return;
  }
  const auto folder = rex::filesystem::GetExecutableFolder() / "shaders";
  std::error_code ec;
  std::filesystem::create_directories(folder, ec);
  std::ofstream(folder / fmt::format("{}_{:016x}.bin", vertex ? "vs" : "ps", hash), std::ios::binary)
      .write(reinterpret_cast<const char*>(blob.data()), std::streamsize(blob.size()));
}

}  // namespace

// void XGRegisterVertexShader(D3DVertexShader*, void *pPhysicalPart) [0x829DD270]
REX_EXTERN(__imp__sub_829DD270);
REX_HOOK_RAW(sub_829DD270) {
  const uint32_t object = ctx.r3.u32;
  if (::banjo::renut_gpu::Active())
    renut_native_sub_829DD270(ctx, base);
  else
    __imp__sub_829DD270(ctx, base);
  DumpShaderObject(base, object, true);
}

// void XGRegisterPixelShader(D3DPixelShader*, void *pPhysicalPart) [0x829DCF98]
REX_EXTERN(__imp__sub_829DCF98);
REX_HOOK_RAW(sub_829DCF98) {
  const uint32_t object = ctx.r3.u32;
  if (::banjo::renut_gpu::Active())
    renut_native_sub_829DCF98(ctx, base);
  else
    __imp__sub_829DCF98(ctx, base);
  DumpShaderObject(base, object, false);
}
