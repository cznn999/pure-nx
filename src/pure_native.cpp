// pure-nx (2026-11-09): native replacements of hot guest leaf functions (nfsmw-nx's "native replacements", its
// docs/performance-history.md step 7). Each one computes exactly what the recompiled function computes (the same
// helpers for every float operation, the same order) and leaves the same registers and memory behind; it only drops
// the recompiled overhead (byte-reversed vector round trips through the stack, per-byte table loads, loop
// bookkeeping in the guest context). pure_native_off = true turns them all back into the recompiled originals.
// Hot list: pure_016's host profile of the game thread in races (sub_82951C70 2.1 %, sub_8296AF40 0.9 %,
// the C runtime's memcpy sub_82F69070 0.5 % with its job-worker share; 1,333 call sites through its thunks).
// After adding or removing a hook here: python tools\direct_calls.py --redo --gen generated\default.
#include "../generated/default/pure_pch.h"

#include <arm_acle.h>

#include <rex/cvar.h>
#include <rex/hook.h>

REXCVAR_DEFINE_BOOL(pure_native_off, false, "Pure",
                    "Run the recompiled originals of the native replacements in src/pure_native.cpp instead")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REX_EXTERN(__imp__sub_8296AF40);
REX_EXTERN(__imp__sub_82951C70);
REX_EXTERN(__imp__sub_82F69070);

namespace {

// CRC-32 (reflected polynomial 0xEDB88320, no inversion) of n bytes: the ARMv8 CRC32 instructions compute the same
// update as the guest's table loop (acc = table[(acc ^ byte) & 0xFF] ^ (acc >> 8)).
__attribute__((target("+crc"))) uint32_t Crc32Bytes(uint32_t crc, const uint8_t *p, uint64_t n) {
  for (; n >= 8; n -= 8, p += 8) {
    uint64_t v;
    std::memcpy(&v, p, 8);  // little-endian: the bytes in memory order
    crc = __crc32d(crc, v);
  }
  for (; n; --n, ++p)
    crc = __crc32b(crc, *p);
  return crc;
}

}  // namespace

// sub_8296AF40 (r3 data, r4 byte count, r5 seed) -> r3: CRC-32 with the standard table at 0x820B7808 (checked
// against the image: table[i] is the reflected 0xEDB88320 remainder of i for all 256). Called from the physics and
// resource code (825xxxxx, 828Dxxxx).
REX_HOOK_RAW(sub_8296AF40) {
  if (REXCVAR_GET(pure_native_off)) {
    __imp__sub_8296AF40(ctx, base);
    return;
  }
  const uint32_t data = ctx.r3.u32;
  const uint64_t n = ctx.r4.u64;
  if (n == 0) {  // beq: r11 = r3, r3 = r5, nothing else written
    ctx.r11.u64 = ctx.r3.u64;
    ctx.r3.u64 = ctx.r5.u64;
    return;
  }
  const uint8_t *p = reinterpret_cast<const uint8_t *>(REX_RAW_ADDR(data));
  // All bytes but the last through the CRC instructions, the last one as the guest does, for its registers.
  const uint32_t prev = Crc32Bytes(ctx.r5.u32, p, n - 1);
  const uint32_t last = p[n - 1];
  const uint32_t r8 = prev & 0xFF, r6 = prev >> 8, r7 = r8 ^ last;
  const uint32_t r3 = REX_LOAD_U32(0x820B7808u + (r7 << 2));
  const uint32_t crc = r3 ^ r6;
  ctx.r4.u64 = 0;
  ctx.r5.u64 = crc;
  ctx.r6.u64 = r6;
  ctx.r7.u64 = r7;
  ctx.r8.u64 = r8;
  ctx.r9.u64 = last;
  ctx.r10.s64 = int64_t(-2113208320) + 30728;  // lis r10,-32245; addi r10,r10,30728 (0x820B7808)
  ctx.r11.s64 = ctx.r3.s64 + int64_t(n);
  ctx.r3.u64 = crc;
}

// sub_82951C70 (r3 frustum: six planes at +16, a distance per plane written at +112; r4 sphere: centre xyz, radius
// at +12) -> r3 = 1 when the sphere is inside every plane's limit (distance < radius for all six), else 0, at the
// first plane that fails. Per plane: vmsum3fp of plane and centre (the same simde_mm_dp_ps), + the plane's w (fadds),
// stored to +112+4i, compared with the radius. The recompiled loop spilled the dot product through the stack's red
// zone (a byte-reversed 16-byte store, a store card and a load) each plane; here only the last one is stored there.
REX_HOOK_RAW(sub_82951C70) {
  if (REXCVAR_GET(pure_native_off)) {
    __imp__sub_82951C70(ctx, base);
    return;
  }
  const uint32_t frustum = ctx.r3.u32, sphere = ctx.r4.u32;
  PPCVRegister v61{}, v62{}, v63{};
  PPCRegister f0{}, f11{}, f12{}, f13{}, temp{};
  PPCCRRegister cr6{};
  PPCXERRegister xer{};
  int64_t r10 = ctx.r3.s64 + 112, r11 = ctx.r3.s64 + 16;
  uint64_t i = 0;
  uint32_t result;
  ctx.fpscr.enableFlushMode();
  for (;;) {
    simde_mm_store_si128((simde__m128i *)v63.u8,
                         REX_BSWAP128(simde_mm_load_si128((simde__m128i *)REX_RAW_ADDR(sphere & ~0xFu))));
    simde_mm_store_si128((simde__m128i *)v62.u8,
                         REX_BSWAP128(simde_mm_load_si128((simde__m128i *)REX_RAW_ADDR(uint32_t(r11) & ~0xFu))));
    simde_mm_store_ps(v61.f32, simde_mm_dp_ps(simde_mm_load_ps(v62.f32), simde_mm_load_ps(v63.f32), 0xEF));
    f0.f64 = rex::ppc::SingleToDoublePure(REX_LOAD_D_U32(uint32_t(r11), 12));
    f13.f64 = rex::ppc::SingleToDoublePure(v61.u32[3]);  // the word the guest reads back from the red zone
    f12.f64 = rex::ppc::AddSingleAnyMode(f0.f64, f13.f64);
    temp.f32 = rex::ppc::DoubleToSingleAnyMode(f12.f64);
    REX_STORE_D_U32_C(uint32_t(r10), 0, temp.u32);
    f11.f64 = rex::ppc::SingleToDoublePure(REX_LOAD_D_U32(sphere, 12));
    rex::ppc::CompareAnyMode(cr6, f12.f64, f11.f64);
    if (!cr6.lt) {
      result = 0;
      break;
    }
    ++i;
    r10 += 4;
    r11 += 16;
    if (i >= 6) {
      result = 1;
      break;
    }
  }
  (void)xer;
  // The red zone as the guest leaves it (its last spill), and every register it writes.
  const uint32_t spill = (ctx.r1.u32 - 16) & ~0xFu;
  REX_CARD(spill), simde_mm_store_si128((simde__m128i *)REX_RAW_ADDR(spill),
                                       REX_BSWAP128(simde_mm_load_si128((simde__m128i *)v61.u8)));
  ctx.r8.s64 = ctx.r1.s64 + -16;
  ctx.r9.u64 = i;
  ctx.r10.s64 = r10;
  ctx.r11.s64 = r11;
  ctx.f0 = f0;
  ctx.f11 = f11;
  ctx.f12 = f12;
  ctx.f13 = f13;
  ctx.v61 = v61;
  ctx.v62 = v62;
  ctx.v63 = v63;
  ctx.r3.u64 = result;
}

// sub_82F69070, the C runtime's memcpy (r3 dst, r4 src, r5 size) -> r3 dst. The guest routine saves r3 at -8(r1),
// copies forward (bytes to alignment, then words / doublewords with dcbt prefetches) and reloads r3 from the slot.
// Native: the same save, one host memmove, the store cards of every page written (Renut's exact streams read them),
// r3 unchanged. It writes no other register; the original clobbers r4-r12, so a caller can expect none of them. The
// original still runs (renut-nx's memcalls.h rules) when the copy would differ: a destination after an overlapping
// source (the forward copy smears), a destination over the save slot (the copied bytes would come back in r3), or a
// range in the hardware registers (0x7Fxxxxxx), the physical views (0xA0000000 up, aliased) or wrapping 4 GB.
namespace {
inline bool PlainGuestRange(uint32_t x, uint32_t n) {
  const uint32_t e = x + (n - 1);
  if (e < x) return false;
  if (x < 0x7F000000u) return e < 0x7F000000u;
  return x >= 0x80000000u && e < 0xA0000000u;
}
}  // namespace

REX_HOOK_RAW(sub_82F69070) {
  const uint32_t dst = ctx.r3.u32, src = ctx.r4.u32, n = ctx.r5.u32;
  const uint32_t slot = ctx.r1.u32 - 8;
  if (REXCVAR_GET(pure_native_off) || n == 0 || ctx.r5.u64 != n || !PlainGuestRange(dst, n) ||
      !PlainGuestRange(src, n) || (dst > src && dst - src < n) || (dst < slot + 8 && slot < dst + n)) {
    __imp__sub_82F69070(ctx, base);
    return;
  }
  REX_STORE_D_U64(ctx.r1.u32, -8, ctx.r3.u64);  // std r3,-8(r1)
  std::memmove(REX_RAW_ADDR(dst), REX_RAW_ADDR(src), n);
  for (uint32_t page = dst >> 12, last = (dst + n - 1) >> 12; page <= last; ++page)
    REX_CARD(page << 12);
  // ld r3,-8(r1): the value just saved
}
