// banjo-nx (2026-10-17): Renut's renderer on plume's deko3d back end (banjo_renut_deko3d.h, docs/deko3d-renderer.md).
#include "renut_engine/gpu/banjo_renut_deko3d.h"

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <unistd.h>  // ftruncate (LoadCache)

#include <fmt/format.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <zstd.h>

#include "renut_engine/gpu/banjo_renut_tuning.h"
#include "renut_engine/gpu/shaders/shader_cache.h"  // peggle2-nx: MissingGameShaders

#if defined(PLUME_DEKO3D)
#include <plume_deko3d.h>
#endif

// The GLSL package (tools/deko3d_shader_package.py: deko3d_glsl_switch.cpp embeds it); absent from builds made without
// one, hence weak.
extern const uint8_t g_deko3dGlslPackage[] __attribute__((weak));
extern const size_t g_deko3dGlslPackageSize __attribute__((weak));

// dishonored-nx (2026-10-30, the green frame): pixel shaders whose colour output is replaced, to see whether a pass's
// geometry reaches its target at all (the scene colour holds only the smoke, sample log 018).
// dishonored-nx (2026-11-01, from renut-nx's banjo_deko3d_async_variants / _async_cache_reads / background compile
// share / GPU region): plume's Deko3DOptions (InstallShaderCache).
REXCVAR_DEFINE_BOOL(dishonored_deko3d_async_variants, true, "Dishonored/Renderer",
                    "deko3d: a shader variant missing from the cache is compiled (or read from the cache file) in the "
                    "background while its draws use the shader's generic variant (the same pixels); no draw waits for "
                    "a specialised variant. Off: the draw waits for it (the freezes)");
REXCVAR_DEFINE_INT32(dishonored_deko3d_background_compile_share, 50, "Dishonored/Renderer",
                     "deko3d: the share of its core (percent) background shader compiles may use; the uam thread rests "
                     "in proportion after each (a compile a draw waits for ends the rest). 100: no rests");
REXCVAR_DEFINE_BOOL(dishonored_deko3d_gpu_region, false, "Dishonored/Renderer",
                    "deko3d: GPU memory blocks from a region of their own at the heap's top (renut-nx). Off: from the "
                    "heap, as the memory budget expects");
// dishonored-nx (2026-11-01, after renut-nx's banjo_deko3d_compile_wait_boost): plume's Deko3DOptions::compileWaitBoost.
REXCVAR_DEFINE_BOOL(dishonored_deko3d_compile_boost, true, "Dishonored/Renderer",
                    "deko3d: while the render thread waits for a shader compile (the game frozen), the console in "
                    "boost mode (CPU 1785 MHz), normal again when it is done: the compile freezes take about half as "
                    "long. Background compiles never boost");
REXCVAR_DEFINE_STRING(dishonored_diag_ps_flat, "", "Dishonored/Diagnostics",
                      "Diagnostic: pixel shaders (comma-separated hex hashes, as the [drawlog] lines name them, or *) "
                      "whose SV_Target0 is replaced at the end of main: magenta, or with dishonored_diag_ps_flat_mode "
                      "1 the shader's colour with alpha 1 and NaN/negative/infinite channels shown as red/green/blue");
// dishonored-nx (2026-10-30, the bright edges): every game vertex shader's gl_Position invariant.
REXCVAR_DEFINE_BOOL(dishonored_vs_invariant, true, "Dishonored/Renderer",
                    "deko3d: the game's vertex shaders declare 'invariant gl_Position', so the depth pass's and the "
                    "colour passes' shaders compute a vertex's position alike (no fused multiply-adds in one and not "
                    "the other): their silhouettes matched pixel for pixel, as on the Xenos");
REXCVAR_DEFINE_INT32(dishonored_diag_ps_flat_mode, 0, "Dishonored/Diagnostics",
                     "Diagnostic: dishonored_diag_ps_flat's output (0 magenta, 1 the colour checked, 2 c0 (the "
                     "pixel constant 0) as the colour)");

namespace banjo::renut_gpu::deko3d {

namespace {

#pragma pack(push, 1)
struct PackageHeader {
  char magic[4];
  uint32_t version, count, dictSize, tableOffset, dataOffset, reserved[2];
};
struct PackageEntry {
  uint64_t hash;
  uint32_t offset, compressedSize, size, stage;
  uint32_t boolMask[8];
};
#pragma pack(pop)
static_assert(sizeof(PackageEntry) == 56, "tools/deko3d_shader_package.py's entry");

// One GLSL package: the one built into the NRO, or (peggle2-nx, 2026-10-20; sa2-nx 1.0.3) deko3d_glsl.bin beside it,
// which the web installer makes from the player's own game files (a build for distribution carries no translated
// game shader: SA2_RENUT_RELEASE_SHADERS).
struct Package {
  bool ok = false;
  std::vector<uint8_t> file;  // the file's bytes (the built-in package is used in place)
  const uint8_t *base = nullptr;
  size_t size = 0;
  const PackageHeader *header = nullptr;
  const PackageEntry *entries = nullptr;
  const uint8_t *data = nullptr;
  ZSTD_DDict *dict = nullptr;  // none when the package has no dictionary (the installer's: raw zstd frames)
  std::mutex mutex;            // the decompression context
  ZSTD_DCtx *dctx = nullptr;
};

// Index 0: beside the NRO (looked up first), 1: built in.
struct Packages {
  std::once_flag once;
  Package source[2];
};
Packages g_packages;

bool Attach(Package &p, const uint8_t *base, size_t size, const char *what) {
  if (size < sizeof(PackageHeader))
    return false;
  const auto *h = reinterpret_cast<const PackageHeader *>(base);
  const uint64_t table_end = uint64_t(h->tableOffset) + uint64_t(h->count) * sizeof(PackageEntry);
  if (std::memcmp(h->magic, "BDKG", 4) != 0 || h->version != 1 || sizeof(PackageHeader) + uint64_t(h->dictSize) > size ||
      table_end > size || h->dataOffset > size) {
    REXLOG_ERROR("[deko3d] {}: not a GLSL package this build reads", what);
    return false;
  }
  const auto *entries = reinterpret_cast<const PackageEntry *>(base + h->tableOffset);
  for (uint32_t i = 0; i < h->count; ++i) {
    if (uint64_t(h->dataOffset) + entries[i].offset + entries[i].compressedSize > size ||
        (i && entries[i - 1].hash >= entries[i].hash)) {
      REXLOG_ERROR("[deko3d] {}: entry {} out of bounds or out of order", what, i);
      return false;
    }
  }
  p.base = base;
  p.size = size;
  p.header = h;
  p.entries = entries;
  p.data = base + h->dataOffset;
  p.dict = h->dictSize ? ZSTD_createDDict(base + sizeof(PackageHeader), h->dictSize) : nullptr;
  p.dctx = ZSTD_createDCtx();
  p.ok = p.dctx && (p.dict || !h->dictSize);
  return p.ok;
}

Packages &Open() {
  std::call_once(g_packages.once, [] {
    Packages &all = g_packages;
    // peggle2-nx: deko3d_glsl.bin beside the NRO first.
    const std::string path = NextToExecutable("deko3d_glsl.bin").string();
    if (FILE *f = std::fopen(path.c_str(), "rb")) {
      Package &p = all.source[0];
      std::fseek(f, 0, SEEK_END);
      const long n = std::ftell(f);
      std::fseek(f, 0, SEEK_SET);
      if (n > 0) {
        p.file.resize(size_t(n));
        if (std::fread(p.file.data(), 1, p.file.size(), f) != p.file.size())
          p.file.clear();
      }
      std::fclose(f);
      if (Attach(p, p.file.data(), p.file.size(), path.c_str()))
        REXLOG_INFO("[deko3d] GLSL package {}: {} shaders, {} KB", path, p.header->count, p.size >> 10);
    }
    if (&g_deko3dGlslPackageSize != nullptr && g_deko3dGlslPackageSize >= sizeof(PackageHeader) &&
        Attach(all.source[1], g_deko3dGlslPackage, g_deko3dGlslPackageSize, "the NRO's GLSL package"))
      REXLOG_INFO("[deko3d] GLSL package: {} shaders, {} KB in the NRO", all.source[1].header->count,
                  g_deko3dGlslPackageSize >> 10);
    if (!all.source[0].ok && !all.source[1].ok)
      REXLOG_ERROR("[deko3d] no GLSL package (deko3d_glsl.bin beside the NRO, or built in): no game shaders");
  });
  return g_packages;
}

struct Found {
  Package *package = nullptr;
  const PackageEntry *entry = nullptr;
};

Found Find(Packages &all, uint64_t hash) {
  for (Package &p : all.source) {
    if (!p.ok)
      continue;
    const PackageEntry *begin = p.entries, *end = p.entries + p.header->count;
    const PackageEntry *it =
        std::lower_bound(begin, end, hash, [](const PackageEntry &e, uint64_t h) { return e.hash < h; });
    if (it != end && it->hash == hash)
      return {&p, it};
  }
  return {};
}

// ---- the DKSH cache next to the NRO: "BDKC", version, the compiler's tag, then records (u64 key, u32 size, DKSH)
// uam 1.1.0 with SwitchWakerHD's 8 patches and banjo-nx's passes (bnx3, 2026-10-18: with the value numbering).
// banjo-nx (2026-10-20): bnx4 with uam banjo flag 1024 (the 128-bit bone rows) in the default flags: a cache
// compiled before is another compiler's.
constexpr char kCacheTag[16] = "uam110-sw8-bnx4";

constexpr uint32_t kDkshMagic = 0x48534B44u;  // "DKSH", the first word of every record's data

struct DkshCache {
  std::mutex mutex;
  FILE *file = nullptr;
  std::unordered_map<uint64_t, std::pair<long, uint32_t>> index;  // key -> offset of the DKSH, size
  long end = 0;  // the end of the last whole record: where the next one is written
  bool read_only = false;  // a write failed: no more records
  uint64_t added = 0, hits = 0, misses = 0;
  std::string path;
};
DkshCache g_cache;

void LoadCache() {
  DkshCache &c = g_cache;
  // banjo-nx (2026-10-18): the code uam makes with other flags (banjo_deko3d_uam_prmt, banjo_deko3d_uam_gvn off) has a
  // cache of its own.
  const unsigned flags = Deko3DUamFlags();
  c.path = NextToExecutable(flags == 1471u ? std::string("deko3d_shaders.bin")
                                          : "deko3d_shaders_f" + std::to_string(flags) + ".bin").string();
  c.file = std::fopen(c.path.c_str(), "r+b");
  bool fresh = c.file == nullptr;
  if (c.file) {
    char head[24] = {};
    if (std::fread(head, 1, sizeof(head), c.file) != sizeof(head) || std::memcmp(head, "BDKC", 4) != 0 ||
        std::memcmp(head + 8, kCacheTag, sizeof(kCacheTag)) != 0) {
      REXLOG_INFO("[deko3d] DKSH cache {}: another compiler's or unreadable; starting it over", c.path);
      std::fclose(c.file);
      c.file = nullptr;
      fresh = true;
    }
    else {
      std::fseek(c.file, 0, SEEK_END);
      const long length = std::ftell(c.file);
      // Records up to the first that is not whole: its size runs past the end, or its data is not a DKSH (an append
      // the process did not finish). The file is cut there and new records follow the good ones. Before, appends
      // went after such a record, where the next start's walk never found them: Ryujinx runs 20-22 compiled the same
      // ~30 shaders on every start.
      long at = sizeof(head);
      for (;;) {
        uint64_t key;
        uint32_t size, magic;
        if (at + 16 > length || std::fseek(c.file, at, SEEK_SET) != 0 || std::fread(&key, sizeof(key), 1, c.file) != 1 ||
            std::fread(&size, sizeof(size), 1, c.file) != 1 || std::fread(&magic, sizeof(magic), 1, c.file) != 1)
          break;
        if (size < 64 || size > (16u << 20) || at + 12 + long(size) > length || magic != kDkshMagic)
          break;
        c.index[key] = {at + 12, size};
        at += 12 + long(size);
      }
      c.end = at;
      if (at != length) {
        REXLOG_WARN("[deko3d] DKSH cache {}: {} bytes after the last whole record ({} of them) dropped", c.path,
                    length - at, c.index.size());
        std::fflush(c.file);
        if (ftruncate(fileno(c.file), at) != 0)
          REXLOG_WARN("[deko3d] DKSH cache {}: could not be cut; new records overwrite the rest", c.path);
      }
    }
  }
  if (fresh) {
    c.file = std::fopen(c.path.c_str(), "w+b");
    if (c.file) {
      char head[24] = {'B', 'D', 'K', 'C', 1, 0, 0, 0};
      std::memcpy(head + 8, kCacheTag, sizeof(kCacheTag));
      std::fwrite(head, 1, sizeof(head), c.file);
      std::fflush(c.file);
      c.end = sizeof(head);
    }
  }
  REXLOG_INFO("[deko3d] DKSH cache {}: {} variants{}", c.path, c.index.size(), c.file ? "" : " (cannot write it)");
}

}  // namespace

bool PackageAvailable() {
  Packages &all = Open();
  return all.source[0].ok || all.source[1].ok;
}

bool PackageScales() {
  // sa2-nx (2026-10-20, 1.0.3.1): a package made with tools/deko3d_glsl_opt.py's pass scale1 (1.0.3's, from an older
  // installer page) has every texture's scale code folded to 0: drawn above 1x, its shaders would take host texel
  // sizes for guest ones. The passes leave the code's read, "(... >> 26u) & 7u", in every shader that has one (SA2:
  // 5 pixel shaders without scale1, none with it), so a package scales when each package in use has it somewhere.
  static const bool scales = [] {
    Packages &all = Open();
    // pure-nx (2026-11-06): Pure's package (tools/switch_shaders/make_switch_shaders.sh) is made without scale1 and
    // fragscale, but no Pure shader reads a texture's guest size: of the 2839, 4 call textureSize, each for the .z of
    // an array texture only, and none uses texelFetch or an offset, so the code's read is gone from every one and the
    // search below cannot tell this package from a scale1 one. It scales.
    if (all.source[0].ok || all.source[1].ok)
      return true;
    static constexpr char kScaleCode[] = ">> 26u) & 7u";
    bool any = false;
    for (Package &p : all.source) {
      if (!p.ok)
        continue;
      any = true;
      bool found = false;
      std::vector<char> text;
      std::lock_guard<std::mutex> lock(p.mutex);
      for (uint32_t i = 0; i < p.header->count && !found; ++i) {
        const PackageEntry &e = p.entries[i];
        text.resize(e.size);
        const size_t n = p.dict ? ZSTD_decompress_usingDDict(p.dctx, text.data(), e.size, p.data + e.offset,
                                                             e.compressedSize, p.dict)
                                : ZSTD_decompressDCtx(p.dctx, text.data(), e.size, p.data + e.offset, e.compressedSize);
        if (ZSTD_isError(n) || n != e.size)
          continue;
        found = std::search(text.begin(), text.end(), kScaleCode, kScaleCode + sizeof(kScaleCode) - 1) != text.end();
      }
      if (!found) {
        REXLOG_WARN("[deko3d] the {} GLSL package was made for 1280x720 only (pass scale1): the game draws at "
                    "1280x720; make deko3d_glsl.bin again with the installer page for 1920x1080 when docked",
                    &p == &all.source[0] ? "deko3d_glsl.bin" : "built-in");
        return false;
      }
    }
    return any;
  }();
  return scales;
}

#if defined(PLUME_DEKO3D)

// dishonored-nx (2026-10-30): dishonored_diag_ps_flat. The GLSL's main is its last function: the line goes before its
// closing brace, after every write of the output.
// dishonored-nx (2026-10-30): dishonored_vs_invariant. UE3 draws a depth pass, then the colour passes with the depth
// test GEQUAL (no depth bias), each with its own vertex shader: uam compiled the position math of each its own way
// (fused or not), so at a silhouette one shader covered a pixel the other did not, and the pixel kept the clear and the
// later additive passes (the bright, object-coloured edges). The line goes after '#version'.
static void InvariantPosition(std::vector<uint8_t> &blob) {
  static const bool on = REXCVAR_GET(dishonored_vs_invariant);
  if (!on)
    return;
  const size_t at = sizeof(plume::Deko3DShaderBlob);
  std::string glsl(reinterpret_cast<const char *>(blob.data() + at), blob.size() - at);
  if (glsl.find("gl_Position") == std::string::npos || glsl.find("invariant gl_Position") != std::string::npos)
    return;
  const size_t line = glsl.find('\n');
  if (line == std::string::npos || glsl.compare(0, 9, "#version ") != 0)
    return;
  glsl.insert(line + 1, "invariant gl_Position;\n");
  plume::Deko3DShaderBlob header;
  std::memcpy(&header, blob.data(), sizeof(header));
  header.glslSize = uint32_t(glsl.size());
  blob.resize(at + glsl.size());
  std::memcpy(blob.data(), &header, sizeof(header));
  std::memcpy(blob.data() + at, glsl.data(), glsl.size());
}

static void FlattenPixelShader(uint64_t hash, std::vector<uint8_t> &blob) {
  static const std::string list = REXCVAR_GET(dishonored_diag_ps_flat);
  if (list.empty())
    return;
  const std::string name = fmt::format("{:016X}", hash);
  if (list != "*" && list.find(name) == std::string::npos)
    return;
  const size_t at = sizeof(plume::Deko3DShaderBlob);
  std::string glsl(reinterpret_cast<const char *>(blob.data() + at), blob.size() - at);
  if (glsl.find("out_var_SV_Target0") == std::string::npos)
    return;
  const size_t end = glsl.rfind('}');
  if (end == std::string::npos)
    return;
  static const int32_t mode = REXCVAR_GET(dishonored_diag_ps_flat_mode);
  const char *line =
      mode == 1 ? "    { vec4 c = out_var_SV_Target0; bvec3 n = isnan(c.rgb); bvec3 i = isinf(c.rgb);\n"
                  "      out_var_SV_Target0 = (any(n) || any(i) || any(lessThan(c.rgb, vec3(0.0))))\n"
                  "          ? vec4(any(n) ? 1.0 : 0.0, any(lessThan(c.rgb, vec3(0.0))) ? 1.0 : 0.0, any(i) ? 1.0 : 0.0, 1.0)\n"
                  "          : vec4(c.rgb, 1.0); }\n"
      : mode == 2 ? "    out_var_SV_Target0 = vec4(abs(g_UboPixel.v[0u].xyz), 1.0);\n"
                  : "    out_var_SV_Target0 = vec4(1.0, 0.0, 1.0, 1.0);\n";
  if (mode == 2 && glsl.find("g_UboPixel") == std::string::npos)
    return;
  glsl.insert(end, line);
  plume::Deko3DShaderBlob header;
  std::memcpy(&header, blob.data(), sizeof(header));
  header.glslSize = uint32_t(glsl.size());
  blob.resize(at + glsl.size());
  std::memcpy(blob.data(), &header, sizeof(header));
  std::memcpy(blob.data() + at, glsl.data(), glsl.size());
  static int logged = 0;
  if (logged++ < 32)
    REXLOG_INFO("[deko3d] dishonored_diag_ps_flat: ps {} flattened (mode {})", name, mode);
}

std::unique_ptr<plume::RenderShader> CreateGameShader(plume::RenderDevice *device, uint64_t hash) {
  Packages &all = Open();
  const Found found = Find(all, hash);
  if (!found.entry || !device) {
    static int logged = 0;
    if ((all.source[0].ok || all.source[1].ok) && logged++ < 16)
      REXLOG_ERROR("[deko3d] shader {:016X} is not in the GLSL package (deko3d_glsl.bin beside the NRO: made by the "
                   "installer from your game files?)", hash);
    return nullptr;
  }
  Package &p = *found.package;
  const PackageEntry *e = found.entry;
  std::vector<uint8_t> blob(sizeof(plume::Deko3DShaderBlob) + e->size);
  plume::Deko3DShaderBlob header;
  header.stage = e->stage;
  header.glslSize = e->size;
  header.dkshSize = 0;
  std::memcpy(blob.data(), &header, sizeof(header));
  {
    std::lock_guard<std::mutex> lock(p.mutex);
    const size_t n = p.dict ? ZSTD_decompress_usingDDict(p.dctx, blob.data() + sizeof(header), e->size,
                                                         p.data + e->offset, e->compressedSize, p.dict)
                            : ZSTD_decompressDCtx(p.dctx, blob.data() + sizeof(header), e->size, p.data + e->offset,
                                                  e->compressedSize);
    if (ZSTD_isError(n) || n != e->size) {
      REXLOG_ERROR("[deko3d] shader {:016X}: the GLSL did not unpack ({})", hash,
                   ZSTD_isError(n) ? ZSTD_getErrorName(n) : "short");
      return nullptr;
    }
  }
  if (e->stage == 4)
    FlattenPixelShader(hash, blob);  // dishonored-nx (2026-10-30): dishonored_diag_ps_flat
  else if (e->stage == 0)
    InvariantPosition(blob);  // dishonored-nx (2026-10-30): dishonored_vs_invariant
  auto shader = device->createShader(blob.data(), blob.size(), "main", plume::RenderShaderFormat::DEKO3D);
  if (shader)
    shader->setName(fmt::format("{}_{:016X}", e->stage == 4 ? "ps" : "vs", hash));
  return shader;
}

// dishonored-nx (2026-11-01): the four-core mode's state (sdk/src/core/threading_switch.cpp), logged here: main()'s line
// comes before the log file opens.
extern "C" void RexSwitchSystemCoreDescribe(char *out, size_t out_len);

void InstallShaderCache() {
  {
    char cores[256] = {};
    RexSwitchSystemCoreDescribe(cores, sizeof(cores));
    REXLOG_INFO("[cores] {}", cores);
  }
  // The back end's messages into this log (plume writes them to stderr otherwise), and into deko3d.log next to the NRO,
  // flushed at each line: the log's buffer is lost when the process dies.
  static FILE *diag = std::fopen(NextToExecutable("deko3d.log").string().c_str(), "w");
  static std::mutex diag_mutex;
  plume::SetDeko3DLogCallback([](const char *message) {
    REXLOG_INFO("[deko3d] {}", message);
    std::lock_guard<std::mutex> lock(diag_mutex);
    if (diag) {
      std::fprintf(diag, "%s\n", message);
      std::fflush(diag);
    }
  });
  // banjo-nx (2026-10-18, GPU bandwidth): depth compression and Zcull, which the device and its textures take.
  plume::Deko3DOptions options;
  options.depthCompression = Deko3DDepthCompressionOn();
  options.zcull = Deko3DZcullOn();
  options.vsOutputs = Deko3DVsOutputsOn();
  options.uamFlags = Deko3DUamFlags();
  options.mergeBarriers = Deko3DBarrierMergeOn();  // banjo-nx (2026-10-19, GPU)
  options.leanBarriers = Deko3DBarrierLeanOn();
  // The uam thread on the background compile core, below the game's threads (2026-10-18).
  options.compileCore = Deko3DCompileCore();
  options.compilePriority = Deko3DCompilePriority();
  options.compileWaitBoost = REXCVAR_GET(dishonored_deko3d_compile_boost);  // dishonored-nx (2026-11-01)
  // dishonored-nx (2026-11-01, from renut-nx 1.0.0's plume deko3d back end, ChanseyIsTheBest): a specialised shader
  // variant missing from the cache is compiled (or read from the cache file) on the uam thread in the background while
  // its draws use the shader's generic variant (the same pixels); the pipeline takes its own variant once it is done.
  options.asyncVariants = REXCVAR_GET(dishonored_deko3d_async_variants);
  options.asyncCacheReads = REXCVAR_GET(dishonored_deko3d_async_variants);
  options.backgroundCompileShare = REXCVAR_GET(dishonored_deko3d_background_compile_share);
  plume::Deko3DSetOptions(options);
  // The GPU region (renut-nx's: memory blocks from a region at the heap's top) off: the memory budget (backend.cpp,
  // QueryGpuMemory) reads the heap, as the blocks came from it before.
  plume::Deko3DSetGpuRegion(REXCVAR_GET(dishonored_deko3d_gpu_region));
  plume::Deko3DSetProvisionalCount(true);  // a draw asks its pipeline only while some pipeline is provisional
  // dishonored-nx (2026-11-01, the menu's button icons stretched over the screen): renut-nx's plume skips a draw's
  // texture binds when the shared-constant lines holding its texture words were not marked written
  // (Deko3DSharedLinesWritten), which only renut-nx's renderer calls: here every draw kept the previous draw's textures.
  plume::Deko3DSetTextureSkip(false);
  LoadCache();
  // The index only (asyncCacheReads): whether the cache file holds a variant, without reading it.
  plume::SetDeko3DShaderCacheContains([](uint64_t key) {
    DkshCache &c = g_cache;
    std::lock_guard<std::mutex> lock(c.mutex);
    return c.file != nullptr && c.index.count(key) != 0;
  });
  plume::SetDeko3DShaderCache(
      [](uint64_t key, std::vector<uint8_t> &dksh) {
        DkshCache &c = g_cache;
        std::lock_guard<std::mutex> lock(c.mutex);
        auto it = c.index.find(key);
        if (it == c.index.end() || !c.file) {
          c.misses++;
          return false;
        }
        dksh.resize(it->second.second);
        uint32_t magic = 0;
        if (std::fseek(c.file, it->second.first, SEEK_SET) != 0 ||
            std::fread(dksh.data(), 1, dksh.size(), c.file) != dksh.size() ||
            (std::memcpy(&magic, dksh.data(), sizeof(magic)), magic != kDkshMagic)) {
          c.misses++;
          c.index.erase(it);
          return false;
        }
        c.hits++;
        return true;
      },
      [](uint64_t key, const std::vector<uint8_t> &dksh) {
        DkshCache &c = g_cache;
        std::lock_guard<std::mutex> lock(c.mutex);
        if (!c.file || c.read_only || c.index.count(key) || dksh.size() < 64)
          return;
        // One write of the whole record at the end of the last whole one, then flushed: a record cut short (the game
        // closed while writing) is the last one, and the next start drops it (LoadCache).
        const uint32_t size = uint32_t(dksh.size());
        std::vector<uint8_t> record(12 + dksh.size());
        std::memcpy(record.data(), &key, sizeof(key));
        std::memcpy(record.data() + 8, &size, sizeof(size));
        std::memcpy(record.data() + 12, dksh.data(), dksh.size());
        if (std::fseek(c.file, c.end, SEEK_SET) == 0 &&
            std::fwrite(record.data(), 1, record.size(), c.file) == record.size() && std::fflush(c.file) == 0) {
          // peggle2-nx (2026-10-20): fflush only hands the record to the FS service; fsync (libnx: fsFileFlush) has it
          // written out. Without it the last records stayed in Ryujinx's file buffer and were lost when the process was
          // closed (every run compiled the same variant again: 8 of 9 records in the file). Once per new record.
          fsync(fileno(c.file));
          c.index[key] = {c.end + 12, size};
          c.end += long(record.size());
          c.added++;
        }
        else {
          // The SD card full or failing: the records so far keep being read; no more are written (a part of this
          // one may be in the file, after c.end, and the next start drops it).
          REXLOG_WARN("[deko3d] DKSH cache {}: a record could not be written; no more are", c.path);
          c.read_only = true;
        }
      });
}

std::string Report() {
  const plume::Deko3DStats &st = plume::GetDeko3DStats();
  const uint64_t compiles = st.shaderCompiles;
  return fmt::format("deko3d: {} draws ({} through the address window), {} pipeline binds ({} state binds); uam {} compiles (avg {:.0f} ms, {} failed), "
                     "DKSH cache {} hits, {} added; memory: images {} MB, buffers {} MB, code {} KB, commands {} KB; "
                     "barriers: {} primitives, {} fragments, {} full ({} invalidating textures); copy and 2D engines: {} "
                     "buffer copies, {} texture uploads, {} readbacks, {} image copies, {} resolves, {} blits",
                     st.draws.load(), st.windowDraws.load(), st.pipelineBinds.load(), st.stateBinds.load(), compiles,
                     compiles ? double(st.shaderCompileNs.load()) / 1e6 / double(compiles) : 0.0,
                     st.shaderCompileFailures.load(), g_cache.hits, g_cache.added, st.imageBytes.load() >> 20,
                     st.bufferBytes.load() >> 20, st.codeBytes.load() >> 10, st.commandBytes.load() >> 10,
                     st.barriersPrimitives.load(), st.barriersFragments.load(), st.barriersFull.load(),
                     st.barriersImageInvalidate.load(), st.copyBuffer.load(), st.copyUpload.load(), st.copyReadback.load(),
                     st.copyImage.load(), st.copyResolve.load(), st.copyBlit.load());
}

#else

std::unique_ptr<plume::RenderShader> CreateGameShader(plume::RenderDevice *, uint64_t) { return nullptr; }
void InstallShaderCache() {}
std::string Report() { return {}; }

#endif

size_t MissingGameShaders() {
  Packages &all = Open();
  size_t missing = 0;
  for (size_t i = 0; i < g_shaderCacheEntryCount; ++i)
    missing += Find(all, g_shaderCacheEntries[i].hash).entry ? 0 : 1;
  return missing;
}

bool BoolSpecMask(uint64_t hash, uint32_t mask[8]) {
  const Found found = Find(Open(), hash);
  if (!found.entry)
    return false;
  std::memcpy(mask, found.entry->boolMask, sizeof(found.entry->boolMask));
  return true;
}

void SaveShaderCache() {
  std::lock_guard<std::mutex> lock(g_cache.mutex);
  if (g_cache.file) {
    std::fflush(g_cache.file);
    fsync(fileno(g_cache.file));  // banjo-nx (2026-10-20): to the card, not only to the file system service
  }
}

}  // namespace banjo::renut_gpu::deko3d
