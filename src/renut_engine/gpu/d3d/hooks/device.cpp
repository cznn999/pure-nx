/**
 * @file    gpu/d3d/hooks/device.cpp
 * @brief   Device lifecycle hooks: creation and release, the command ring,
 *          fences, command buffer recording and Swap.
 *
 * The command ring is a stub, as in reblue: m_pRing points at a scratch block
 * that KickOff simply rewinds, and none of the hooked D3D calls writes packets
 * (d3d_native.h). The renderer consumes D3D's API-level state instead (the
 * device's constant/packet shadows and the hooked state-setting and draw calls).
 *
 * The renderer runs on its own thread (render_thread.h). KickOff and Swap
 * queue the end of a batch of work and its fence, which completes once the
 * render thread has read the batch's data; D3D's fence waits wait for that
 * (BlockOnFence_hook), and Swap waits when the game gets a frame too far ahead.
 */
#include <rex/frame_latency.h>  // banjo-nx (2026-10-19)
#include "renut_engine/display_sync.h"
#include "renut_engine/gpu/render/pipeline.h"  // banjo-nx (2026-10-20): SyncPipelineListLocked
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <mutex>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include <rex/hook.h>

#include "renut_engine/gpu/banjo_renut_tuning.h"  // banjo-nx: CountFenceQuery
#include "renut_engine/gpu/banjo_renut_checks.h"  // banjo-nx (package 27c): QueueKickFlush
#include "renut_engine/gpu/banjo_renut_select.h"
#include "renut_engine/gpu/sa2_renut_pacing.h"  // sa2-nx
#include "renut_engine/gpu/banjo_renut_capture.h"  // sa2-nx: lockstep captures
#include "sa2_capture.h"                            // sa2-nx: lockstep captures
#include "me/sa2_render_mode.h"                     // sa2-nx (1.0.3.1): the PC's docked test toggles

#include <rex/cvar.h>
#include <rex/kernel/xboxkrnl/video.h>
#include <rex/ppc.h>
#include <rex/system/xvideo.h>
#include <rex/types.h>

#include "renut_engine/perf/frame_stats.h"
#include "renut_engine/gpu/guest/callbacks.h"
#include "renut_engine/gpu/guest/command_buffers.h"
#include "renut_engine/gpu/d3d/d3d.h"
#include "renut_engine/gpu/d3d/d3d_native.h"
#include "renut_engine/gpu/render/draw.h"
#include "renut_engine/gpu/guest/guest_memory.h"
#include "renut_engine/gpu/guest/guest_records.h"
#include "renut_engine/gpu/log.h"
#include "renut_engine/gpu/backend/memory_report.h"
#include "renut_engine/gpu/backend/queued.h"
#include "renut_engine/gpu/backend/render_thread.h"
#include "renut_engine/gpu/render/surfaces.h"
#include "renut_engine/gpu/render/occlusion.h"  // dishonored-nx (2026-10-31)
#include "renut_engine/gpu/render/textures.h"
#include "renut_engine/gpu/backend/video.h"

using namespace renut::gpu;

// sa2-nx: frames the game has presented (src/sa2_d3d_trace.cpp; the test recorder waits on it).
extern std::atomic<uint64_t> g_sa2_game_frames;
REXCVAR_DECLARE(bool, sa2_renut_gamma_ramp);  // sa2-nx (2026-10-20): backend/present.cpp
// sa2-nx (2026-10-20, sa2_renut_core_layout): the SDK's core layout (system/xthread.cpp).
extern "C" uint32_t RexSetGuestThreadCoreLayout(int main_core, uint32_t others_mask, int others_preferred);
// pure-nx (2026-11-07): per start routine (system/xthread.cpp).
extern "C" bool RexSetGuestThreadCoreOverride(uint32_t start_address, int preferred, uint32_t mask);

// Kernel exports the device code uses.
REX_IMPORT(__imp__rex_GetCurrentThreadId, GuestGetCurrentThreadId, u32());
REX_IMPORT(__imp__RtlInitializeCriticalSection, RtlInitializeCriticalSection, void(u32));
REX_IMPORT(__imp__KeQueryPerformanceFrequency, KeQueryPerformanceFrequency, u32());
REX_IMPORT(__imp__KeGetCurrentProcessType, KeGetCurrentProcessType, u32());
REX_IMPORT(__imp__VdGetCurrentDisplayGamma, VdGetCurrentDisplayGamma, void(u32, u32));

namespace {

// Scratch space packets go to: the inline writers in N&B's own code (D3D's
// header helpers), which reserve against m_pRingGuarantee. Large enough for
// the biggest single reservation; the guarantee margin is what inline writers
// assume is free without calling KickOff.
constexpr u32 kRingSinkSize = 4u << 20;
constexpr u32 kRingSinkGuaranteeMargin = 256u << 10;
// Scratch handed out for inline data (RingBufferDeviceAllocate), reused in a
// circle: nothing reads it either.
constexpr u32 kRingDataSize = 1u << 20;

// CDevice-private state Direct3D_CreateDevice (0x8264DF28) sets up, as byte
// offsets into the device allocation. sa2-nx (2026-10-20): SA2's (d3d.h),
// read from its Direct3D_CreateDevice sub_8254FC10, sub_8255C808, sub_8255C878
// and the hardware path sub_8255CDA0; N&B's in brackets.
// Byte whose 0x80 bit marks a device without GPU hardware (its device-type-2
// path sets it); D3D's code skips hardware work for it.
constexpr u32 kDeviceOffsetFlags = 10940;  // pure-nx: N&B's (SA2 11068)
constexpr u8 kDeviceFlagNoHardware = 0x80;
// Byte whose 0x2 bit makes CDevice::SetFence (0x82278528) write each fence's
// completion value itself instead of waiting for the GPU to. The title device
// runs without it once created: its fences complete on the render thread.
constexpr u32 kDeviceOffsetFlags2 = 10941;  // pure-nx: N&B's (SA2 11069)
constexpr u8 kDeviceFlagImmediateFences = 0x02;
// Pointer to the completed-fence word IsFencePending / BlockOnFence read.
constexpr u32 kDeviceOffsetFenceCompletion = 10896;  // pure-nx: N&B's (SA2 11024)
// Nonzero while a KickOff is in progress (BlockOnFence does not kick then).
// sa2-nx: the word SA2's CDevice::BlockOnFence (sub_8254D6E8) tests [13232].
constexpr u32 kDeviceOffsetInKickOff = 13232;  // pure-nx: Pure's BlockOnFence 0x829E9530 (SA2 13600)
// Side buffer (0x12C0 bytes) every device gets.
constexpr u32 kDeviceOffsetSideBuffer = 16728;  // pure-nx: Pure's constructor 0x82A043B8 (SA2 17136)
constexpr u32 kSideBufferSize = 0x12C0;
// Set to -1 by the device-type-2 path.
constexpr u32 kDeviceOffsetsMinusOne[] = {21560, 21564, 14936};  // pure-nx: N&B's (SA2 21968, 21972, 15340)
constexpr u32 kDeviceTypeNoHardware = 2;
// Copied from the global qword at 0x82F39B38 into every device.
constexpr u32 kDeviceOffsetGlobalQword = 10880;  // pure-nx: N&B's (SA2 11008)
// pure-nx: Pure's constructor 0x82A043B8 (dishonored-nx 0x833BCA50, sub_82A527B0; SA2 0x82980268; N&B 0x82F39B38).
constexpr u32 kGlobalQwordVa = 0x83637618;
// Hardware-path fields: two critical sections, QueryPerformanceFrequency and
// its reciprocal as floats, and 41 words set to -1.
constexpr u32 kDeviceOffsetCriticalSection0 = 14944;  // pure-nx: Pure's 0x82A04440 (SA2 15348)
constexpr u32 kDeviceOffsetCriticalSection1 = 14972;  // pure-nx: Pure's 0x82A04440 (SA2 15376)
constexpr u32 kDeviceOffsetPerfFrequency = 21588;  // pure-nx: Pure's 0x82A04440 (SA2 21996)
constexpr u32 kDeviceOffsetTitleMinusOne = 22920;  // pure-nx: by the -460 of the fields around it (SA2 23380); the native creation path only
// Import slot holding the address of the kernel's VdGlobalDevice variable.
// dishonored-nx: Dishonored's hardware path (sub_82A5F9A0) stores the device through 0x820007AC (VdGlobalDevice, a
// title process) or 0x820007B0 (VdGlobalXamDevice, KeGetCurrentProcessType() == 2); the creation hook picks the same
// one (SA2's 0x82000710, N&B's 0x820007FC).
// pure-nx: Pure's hardware path (0x82A04440) stores the device through the slot at 0x83850B84 only.
u32 kVdGlobalDeviceImportSlot = 0x83850B84;

constexpr u32 kE_OUTOFMEMORY = 0x8007000Eu;

u32 g_ring_sink = 0;
u32 g_ring_data = 0;
std::atomic<u32> g_ring_data_offset{0};

// Device blocks of released devices, for the next Direct3D_CreateDevice (the
// records heap never frees physical blocks). N&B creates and releases a pair
// of no-hardware devices around every section it loads.
std::mutex g_free_devices_mutex;
std::vector<u32> g_free_devices;

void WriteU32(u8 *device_bytes, u32 offset, u32 value) {
  *reinterpret_cast<be_u32 *>(device_bytes + offset) = value;
}

u32 LoadGuestU32(u32 va) {
  const auto *p = GuestPtr<const be_u32>(va);
  return p ? u32(*p) : 0;
}

// The newest fence queued for completion (the title device's).
std::atomic<u32> g_queued_fence{0};

// What CDevice::SetFence (0x82278528) does at a KickOff: the current fence
// covers the work issued so far, and the device moves on to the next. The
// fence completes when the render thread has read that work's data (and
// written its memexport results): until then the game must not reuse it.
// Without completing no resource tagged with a fence would stop being busy.
void KickFence(u8 *bytes) {
  const u32 fence = *reinterpret_cast<const be_u32 *>(bytes + d3d::kDeviceOffsetFenceTag);
  const u32 completion = *reinterpret_cast<const be_u32 *>(bytes + kDeviceOffsetFenceCompletion);
  WriteU32(bytes, d3d::kDeviceOffsetFenceTag, fence + 2);
  g_queued_fence.store(fence, std::memory_order_release);
  render_thread::QueueFenceCompletion(completion, fence, LastCallback());
}

// IsFencePending's test.
bool FencePending(const u8 *bytes, u32 fence) {
  const u32 tag = *reinterpret_cast<const be_u32 *>(bytes + d3d::kDeviceOffsetFenceTag);
  const u32 completion = *reinterpret_cast<const be_u32 *>(bytes + kDeviceOffsetFenceCompletion);
  const auto *word = GuestPtr<const be_u32>(completion);
  return word && tag - fence < tag - u32(*word);
}

// ---------------------------------------------------------------------------
// Device initialisation (N&B's D3D, natively)
// ---------------------------------------------------------------------------

// D3D::XenonGpu_Initialize (0x82663208): the register image's power-on
// values, everything dirty. (Its register writes go into the ring.)
void XenonGpuInitialize(native::Device d) {
  d.SetQ(0x2E30, 0x00000000FFFFFFFFull);
  for (u32 i = 0; i < 26; ++i)  // texture fetch constants: type 0
    d.SetW(native::off::TextureFetch + i * 24, d.W(native::off::TextureFetch + i * 24) & ~3u);
  for (u32 i = 0; i < 18; ++i) {  // vertex fetch constants: type 1
    const u32 offset = native::off::VertexFetch + i * 8;
    d.SetW(offset, (d.W(offset) & ~3u) | 1);
  }
  d.SetW(0x28BC, 0x20002000);
  d.SetW(0x29D4, 0xFF000);
  d.SetW(0x2A14, 0x10);
  d.SetW(native::off::ClipControl, d.W(native::off::ClipControl) | 0x80000);
  d.SetW(0x2984, 0xE);
  d.SetW(0x2A10, 0xE);
  d.SetW(0x296C, 8);
  d.SetW(0x29D8, 0xFF100);
  d.SetW(0x2954, 4);
  // PA_SU_SC_MODE_CNTL vtx_window_offset_enable: one-tile draws are in screen
  // space (see draw.cpp, ComputeViewport).
  d.SetW(native::off::ModeControl, d.W(native::off::ModeControl) | 0x10000);
  d.SetW(0x28CC, 0xFFFFFF);
  d.SetW(0x29C0, 4);
  d.SetW(0x2A48, 2);
  for (u32 word = 0; word < 5; ++word)
    d.SetPending(word, ~u64(0));
  d.SetW(0x2AA4, 0xE);
}

// D3D::InitializeApiState (0x826597C8).
void InitializeApiState(native::Device d) {
  const u32 present_interval = d.W(0x351C);
  const u32 behavior_flags2 = d.W(0x5DAC);
  native::InitializeStateTables(d);
  d.SetW(0x2FA0, 5);
  d.SetW(0x2FA4, 1);
  native::SetScreenExtentQueryMode(d, 2);  // D3DSEQM_PRECLIP
  d.SetW(0x3090, 2);
  d.SetW(0x2FD8, 0x2A23B9);
  d.SetH(0x2FD4, 0);
  d.SetH(0x2FD6, 0);
  d.SetB(0x2FDC, 0);
  d.SetB(0x2FDD, 0);
  d.SetB(0x2FDE, 0);
  d.SetW(0x2FE4, ~0u);
  // Three zero bytes; N&B leaves the fourth from its stack.
  d.SetW(0x2FE8, 0);
  d.SetW(0x2FE0, 0x00FF0000);
  d.SetW(0x2FB8, 1);
  d.SetW(0x2FBC, 0);
  d.SetW(0x2FD0, 0);
  // The default vertex declaration is the device's own, at +0x2FA0.
  d.SetW(0x2E2C, d.va + 0x2FA0);
  d.Dirty(2, 0x80000);
  d.SetW(0x351C, present_interval);
  const u32 flags2 = d.W(0x5DAC);
  d.SetW(0x5DAC, (behavior_flags2 & 1) ? (flags2 | 1) : (flags2 & ~1u));
  d.SetW(native::off::PredicationSet, 0);
  native::UpdateTilePredicated(d);
  d.SetW(native::off::Predication, ~0u);
  d.SetW(native::off::RunPredicationState, 0);
  d.SetW(native::off::TilePredication, 0);
  d.SetW(native::off::RunPredication, 0);
  if (!(d.B(native::off::Flags) & native::kFlagsNoHardware))
    native::SetShaderGPRAllocation(d, 0, 0, 0);
}

// sub_8265F688: the scaler parameters (D3DPRESENT_PARAMETERS +0x60) the
// device keeps. N&B's code then derives the default scaling for the video
// mode and passes it to VdCallGraphicsNotificationRoutines: the display's.
void SetScalerParameters(native::Device d, const be_u32 *scaler) {
  u32 copy[14] = {};
  for (u32 i = 0; i < 6; ++i)
    copy[i] = scaler[i];
  copy[6] = copy[10] = scaler[6] ? 3 : 0;
  for (u32 i = 0; i < 14; ++i)
    d.SetW(0x359C + i * 4, copy[i]);
  d.SetB(native::off::Flags2, d.B(native::off::Flags2) | 0x10);
}

void SetGammaRamp(native::Device d, const u16 *ramp);
void SetPWLGamma(native::Device d, const u16 *pwl);

// BOOL D3D::InitializePresentationParameters(CDevice*, D3DPRESENT_PARAMETERS*)
// [0x82659AA8]: the video mode, the automatic back/depth buffers and the
// presentation state. N&B's parameters (sub_823ED9E8) disable the automatic
// front buffer and depth buffer and ask for the back buffer.
bool InitializePresentationParameters(native::Device d, u32 parameters_va) {
  const auto *p = GuestPtr<const be_u32>(parameters_va);
  if (!p)
    return false;
  const u32 width = p[0];
  const u32 height = p[1];
  const u32 back_format = p[2];
  const u32 multisample = p[4];
  const u32 depth_format = p[10];
  u32 front_format = p[16];
  // The front buffer format, as the scanout reads it.
  if (front_format == 0 || front_format == 0x28280186u)
    front_format = 0x28280106u;
  else if (front_format == 0x18280186u)
    front_format = 0x18280106u;
  else if (front_format == 0x282801B6u)
    front_format = 0x28280136u;
  else if (front_format == 0x182801B6u)
    front_format = 0x18280136u;

  rex::system::X_VIDEO_MODE mode{};
  rex::kernel::xboxkrnl::VdQueryVideoMode(&mode);
  d.SetW(0x5428, u32(mode.display_width));
  d.SetW(0x5430, u32(mode.display_width));
  d.SetW(0x5434, u32(i64(float(mode.refresh_rate) + 0.5f)));
  d.SetW(0x542C, u32(mode.display_height));

  if (!p[15])  // DisableAutoFrontBuffer
    RGPU_ERROR("InitializePresentationParameters: an automatic front buffer is not supported");
  if (!p[14]) {  // DisableAutoBackBuffer
    const u32 surface = native::CreateSurface(width, height, back_format, multisample, 0);
    if (!surface)
      return false;
    d.SetW(native::off::AutoBackBuffer, surface);
    native::SetRenderTarget(d, 0, surface);
  }
  if (p[9]) {  // EnableAutoDepthStencil
    const u32 surface = native::CreateSurface(width, height, depth_format, multisample, 0);
    if (!surface)
      return false;
    d.SetW(native::off::AutoDepthStencil, surface);
    native::SetDepthStencilSurface(d, surface);
  }
  for (u32 i = 0; i < 0x7C / 4; ++i)
    d.SetW(0x34E8 + i * 4, p[i]);
  if ((front_format & 0x3FFC0000u) != 0x24900000u)
    d.SetW(0x3528, (front_format & 0x3F) | 0x28280100u);
  SetScalerParameters(d, p + 0x60 / 4);
  // The default gamma: linear, as a piecewise-linear ramp for the front
  // buffer formats 7 and 0x36 (0x32 counts as 6).
  u32 gamma_format = front_format & 0x3F;
  if (gamma_format == 0x32)
    gamma_format = 6;
  else if (gamma_format == 7)
    gamma_format = 0x36;
  u16 ramp[768];
  if (gamma_format == 7 || gamma_format == 0x36) {
    for (u32 k = 0; k < 128; ++k) {
      const u16 base = u16((0xFFFFu * k) / 0x7F);
      const u16 delta = u16((0xFFFFu * (k + 1)) / 0x7F - base);
      for (u32 c = 0; c < 3; ++c) {
        ramp[c * 256 + k * 2] = base;
        ramp[c * 256 + k * 2 + 1] = delta;
      }
    }
    SetPWLGamma(d, ramp);
  } else {
    for (u32 i = 0; i < 256; ++i) {
      const u16 value = u16((((0x3FFu * i) / 0xFF) & 0x3FF) << 6);
      ramp[i] = ramp[256 + i] = ramp[512 + i] = value;
    }
    SetGammaRamp(d, ramp);
  }
  d.SetW(0x4144, ~0u);
  return true;
}

// The hardware path (D3D__InitializeHardwareDevice 0x82659F30) minus the GPU:
// the device becomes the one D3D internals find through VdGlobalDevice, and
// gets the presentation state and per-device fields they read.
bool InitializeTitleDevice(native::Device d, u32 present_parameters) {
  RtlInitializeCriticalSection(d.va + kDeviceOffsetCriticalSection0);
  RtlInitializeCriticalSection(d.va + kDeviceOffsetCriticalSection1);
  d.SetW(16712, 0);

  // D3D reads the device through the kernel's VdGlobalDevice from any thread
  // (resource locks, blockers).
  if (const u32 global = LoadGuestU32(kVdGlobalDeviceImportSlot))
    *GuestPtr<be_u32>(global) = d.va;

  const float frequency = float(KeQueryPerformanceFrequency());
  const float period = frequency > 0.0f ? 1.0f / frequency : 0.0f;
  d.SetF(kDeviceOffsetPerfFrequency, frequency);
  d.SetF(kDeviceOffsetPerfFrequency + 4, period);
  for (u32 i = 0; i < 41; ++i)
    d.SetW(kDeviceOffsetTitleMinusOne + i * 4, 0xFFFFFFFFu);

  return !present_parameters || InitializePresentationParameters(d, present_parameters);
}

// D3D's CDevice teardown (0x8265A198) for our devices: they carry the
// no-hardware flag, which skips everything but unbinding. The side buffer is
// part of our device block, and N&B never gives a device a query block.
void UninitializeDevice(native::Device d) {
  if (d.W(native::off::Ring))
    native::UnsetAll(d);
}

// HRESULT Direct3D_CreateDevice(UINT Adapter, D3DDEVTYPE DeviceType, void *pUnused,
//     DWORD BehaviorFlags, D3DPRESENT_PARAMETERS *pPresentParameters,
//     IDirect3DDevice9 **ppReturnedDeviceInterface)   [0x8264DF28]
// N&B creates two: type 1 (the title's rendering device) and type 2 (D3D's
// own no-hardware device, which N&B keeps alongside).
u32 Direct3D_CreateDevice_hook(u32 /*adapter*/, u32 device_type, u32 /*unused*/,
                               u32 behavior_flags, u32 present_parameters,
                               mapped_u32 out_device) {
  if (out_device)
    *out_device = 0u;

  // Every D3D object the renderer creates from here on lives in this heap.
  if (!records::Init())
    return kE_OUTOFMEMORY;

  // The device, its side buffer and fence word share one block of our own
  // guest memory (64 KiB aligned, so each part keeps its 0x80 alignment).
  static_assert(d3d::kDeviceAllocSize % d3d::kDeviceAlignment == 0);
  constexpr u32 kFenceOffset = d3d::kDeviceAllocSize + ((kSideBufferSize + 0x7F) & ~0x7Fu);
  constexpr u32 kBlockSize = kFenceOffset + 0x80;
  u32 block_va = 0;
  {
    std::lock_guard lock(g_free_devices_mutex);
    if (!g_free_devices.empty()) {
      block_va = g_free_devices.back();
      g_free_devices.pop_back();
    }
  }
  if (!block_va)
    block_va = records::AllocPhysicalBlock(kBlockSize);
  const u32 device_va = block_va;
  const u32 side_buffer_va = block_va ? device_va + d3d::kDeviceAllocSize : 0;
  const u32 fence_va = block_va ? device_va + kFenceOffset : 0;
  if (!g_ring_sink)
    g_ring_sink = records::AllocPhysicalBlock(kRingSinkSize);
  if (!g_ring_data)
    g_ring_data = records::AllocPhysicalBlock(kRingDataSize);
  if (!device_va || !g_ring_sink || !g_ring_data) {
    RGPU_ERROR("Direct3D_CreateDevice: guest allocation failed");
    return kE_OUTOFMEMORY;
  }
  std::memset(GuestPtr<u8>(device_va), 0, kBlockSize);
  std::memset(GuestPtr<u8>(g_ring_sink), 0, kRingSinkSize);

  const native::Device d = native::DeviceAt(device_va);
  auto *device = GuestPtr<d3d::Device>(device_va);
  auto *bytes = d.bytes;
  const bool title_device = device_type != kDeviceTypeNoHardware;
  device->m_ReferenceCount = 1u;
  // Fields every device gets.
  WriteU32(bytes, kDeviceOffsetGlobalQword, LoadGuestU32(kGlobalQwordVa));
  WriteU32(bytes, kDeviceOffsetGlobalQword + 4, LoadGuestU32(kGlobalQwordVa + 4));
  WriteU32(bytes, d3d::kDeviceOffsetCreationThreadId, GuestGetCurrentThreadId());
  WriteU32(bytes, d3d::kDeviceOffsetIsTitleProcess, 0);  // KeGetCurrentProcessType() != 2
  WriteU32(bytes, kDeviceOffsetSideBuffer, side_buffer_va);
  WriteU32(bytes, kDeviceOffsetFenceCompletion, fence_va);
  if (title_device) {
    // The original's default when no ring configuration is requested.
    if (!(behavior_flags & 0x100) && !(behavior_flags & 0x3F000000))
      behavior_flags |= 0xC000000;
    WriteU32(bytes, d3d::kDeviceOffsetBehaviorFlags, behavior_flags);
    WriteU32(bytes, 21560, 0xFFFFFFFFu);
    WriteU32(bytes, 21564, 0xFFFFFFFFu);
  } else {
    for (u32 offset : kDeviceOffsetsMinusOne)
      WriteU32(bytes, offset, 0xFFFFFFFFu);
  }
  // Both run without GPU hardware: no hardware setup in the API state, and
  // fences complete as they are written.
  bytes[kDeviceOffsetFlags] |= kDeviceFlagNoHardware;
  bytes[kDeviceOffsetFlags2] |= kDeviceFlagImmediateFences;
  native::RewindRing(d);

  XenonGpuInitialize(d);
  if (title_device && !InitializeTitleDevice(d, present_parameters)) {
    RGPU_ERROR("Direct3D_CreateDevice: InitializePresentationParameters failed");
    return kE_OUTOFMEMORY;
  }
  InitializeApiState(d);
  if (title_device)
    native::ResetAllState(d);
  native::RewindRing(d);
  // From here the title device's fences complete on the render thread
  // (KickFence). The no-hardware device only records command buffers.
  if (title_device)
    bytes[kDeviceOffsetFlags2] &= u8(~kDeviceFlagImmediateFences);

  if (out_device)
    *out_device = device_va;
  if (title_device)
    Video::SetGuestDevice(device_va);
  RGPU_INFO("Guest D3D device (type {}) created at {:#010x}", device_type, device_va);
  return 0;  // S_OK
}

// ULONG D3DDevice_Release(D3DDevice*) [0x8264E080]
// As the original, but on the last release the device block goes back to our
// free list instead of N&B's heap (the original frees the side buffer and the
// raw allocation Direct3D_CreateDevice kept at device - 4).
u32 D3DDevice_Release_hook(u32 device_va) {
  auto *device = GuestPtr<d3d::Device>(device_va);
  if (!device)
    return 0;
  const u32 count = device->m_ReferenceCount;
  if (count != 1) {
    device->m_ReferenceCount = count - 1;
    return count - 1;
  }
  FlushDeferredVertices(device_va);
  UninitializeDevice(native::DeviceAt(device_va));
  device->m_ReferenceCount = 0u;
  RGPU_INFO("Guest D3D device at {:#010x} released", device_va);
  // The title device stays the renderer's until another replaces it.
  if (device_va != Video::GuestDevice()) {
    std::lock_guard lock(g_free_devices_mutex);
    g_free_devices.push_back(device_va);
  }
  return 0;
}

// The GPU starts on the kicked draws: the guest buffers they read are read
// then (FlushVertexFillsLocked), before the fence tells the game it may reuse
// them. Memexport results reach guest memory later, once the GPU has finished
// the frame (detail::CompleteMemExportsLocked); draws that read them sooner
// wait for them (DrawStateLocked).
void KickDevice(d3d::Device *device, u32 device_va) {
  FlushDeferredVertices(device_va);
  // banjo-nx (package 27c): the texture fills too (they were read at the submit, after this fence had let the game
  // rewrite them).
  banjo::renut_gpu::checks::QueueKickFlush();
  KickFence(reinterpret_cast<u8 *>(device));
}

// PRING CDevice::KickOff() [0x82210AB0]: hand the ring to the GPU. Nothing
// consumes it here, so rewind it and return the new write pointer. A device
// recording a command buffer (N&B records on worker threads with no-hardware
// devices) would chain its segment into the buffer; its draws are recorded on
// the host (command_buffers.h), so it only rewinds.
u32 KickOff_hook(u32 device_va) {
  auto *device = GuestPtr<d3d::Device>(device_va);
  if (!device)
    return 0;
  if (!IsRecordingCommandBuffer(device_va)) {
    RunReadyCallbacks();
    KickDevice(device, device_va);
  }
  native::RewindRing(native::DeviceAt(device_va));
  return device->m_pRing;
}

// void CDevice::KickOffSegment() [0x822106C8]
void KickOffSegment_hook(u32 device_va) {
  auto *device = GuestPtr<d3d::Device>(device_va);
  if (!device)
    return;
  if (!IsRecordingCommandBuffer(device_va))
    KickDevice(device, device_va);
  native::RewindRing(native::DeviceAt(device_va));
}

// PRING CDevice::BeginRingBig(DWORD Count) [0x821F7C18]: room for Count
// dwords, kicking when the segment is full.
u32 BeginRingBig_hook(u32 device_va, u32 count) {
  const native::Device d = native::DeviceAt(device_va);
  if (!d)
    return 0;
  if (d.W(native::off::Ring) + count * 4 > d.W(native::off::RingLimit))
    native::RewindRing(d);
  return d.W(native::off::Ring);
}

// PRING CDevice::BeginRingManual() [0x82A11158]
u32 BeginRingManual_hook(u32 device_va) {
  const native::Device d = native::DeviceAt(device_va);
  return d ? d.W(native::off::Ring) : 0;
}

// Inline data space (vertex data of inline draws, clear rects): scratch that
// nothing reads, handed out in a circle.
u32 RingData(u32 bytes, u32 alignment) {
  if (!g_ring_data || bytes > kRingDataSize / 2)
    return 0;
  if (alignment < 4 || (alignment & (alignment - 1)))
    alignment = 4;
  u32 offset = g_ring_data_offset.fetch_add(bytes + alignment, std::memory_order_relaxed);
  offset %= kRingDataSize / 2;
  offset = (offset + alignment - 1) & ~(alignment - 1);
  return g_ring_data + offset;
}

// void *D3D::RingBufferDeviceAllocate(CDevice*, DWORD Flags, DWORD *pSize,
//     DWORD Alignment) [0x8222D3E0]
u32 RingBufferDeviceAllocate_hook(u32 /*device*/, u32 /*flags*/, mapped_u32 size, u32 alignment) {
  return size ? RingData(u32(*size), alignment) : 0;
}

// void *CDevice inline data allocation (sub_8222D240): from the ring, or from
// the allocator of the command buffer being recorded.
u32 AllocateInlineData_hook(u32 /*device*/, u32 dwords, u32 alignment) {
  return RingData(dwords * 4, alignment);
}

// HRESULT D3DDevice_SetRingBufferParameters(D3DDevice*,
//     const D3DRING_BUFFER_PARAMETERS*) [0x82657508]: would replace the ring
// (and our sink) with a freshly allocated one.
u32 D3DDevice_SetRingBufferParameters_hook(u32 /*device*/, u32 /*parameters*/) {
  return 0;  // S_OK
}

// void CDevice::BlockOnFence(DWORD Fence, CBlocker::Type, BOOL NoKick)
// [0x8228F558]: every fence wait (D3DDevice_BlockOnFence, resource locks and
// destruction, InvalidateConstants). As the original: kick the fence if no
// KickOff has covered it yet, then wait for it, here for the render thread to
// complete it (KickFence).
// The original polls through CBlocker::Check, which after 5 s without
// progress dumps the GPU's registers and traps.
u32 BlockOnFence_hook(u32 device_va, u32 fence, u32 /*type*/, u32 /*no_kick*/) {
  banjo::renut_gpu::CountFenceQuery();  // banjo-nx (package 27c): the stream checks' design
  auto *bytes = GuestPtr<u8>(device_va);
  if (!bytes || !FencePending(bytes, fence))
    return 0;
  // Devices with immediate fences (the command buffer recorders) complete
  // theirs as they write them; the original returns at once for them too.
  if (bytes[kDeviceOffsetFlags2] & kDeviceFlagImmediateFences)
    return 0;
  const bool covered =
      int32_t(fence - g_queued_fence.load(std::memory_order_acquire)) <= 0;
  if (!covered) {
    if (*reinterpret_cast<const be_u32 *>(bytes + kDeviceOffsetInKickOff))
      return 0;
    KickOff_hook(device_va);
  }
  // The fence may wait for callbacks, which this thread runs as the render
  // thread reaches them.
  const auto waiting = std::chrono::steady_clock::now();
  for (;;) {
    RunReadyCallbacks();
    if (!FencePending(bytes, fence))
      break;
    if (!render_thread::WaitForFenceProgress(std::chrono::milliseconds(2)))
      break;  // no render thread: nothing would complete it
  }
  renut::frame_stats::OnGameThreadWait(
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - waiting).count());
  return 0;
}

// pure-nx (2026-11-06, the black screen after loading a save: the game thread spun here for minutes): Pure's D3D
// function at 829EA500 (called from 15 places in D3D) writes a packet, blocks on the current fence (KickOff when it
// was not sent), then spins until device+0x2B00 is 0. That word counts packets 82A07DD0 put in the ring for the GPU
// to write back (829E92F8 adds to it); on the console the GPU clears it, and only D3D's hang recovery (82A03488,
// through CBlocker::Check) ever stores 0 from the CPU. With no GPU nothing clears it. Here: the same block on the
// fence (the render thread catches up), then the word the GPU would write.
constexpr u32 kPureDeviceOffsetGpuPending = 0x2B00;  // 11008
void PureWaitGpuPending_hook(u32 device_va) {
  auto *bytes = GuestPtr<u8>(device_va);
  if (!bytes)
    return;
  const u32 fence = *reinterpret_cast<const be_u32 *>(bytes + d3d::kDeviceOffsetFenceTag);
  BlockOnFence_hook(device_va, fence, 4, 0);
  *reinterpret_cast<be_u32 *>(bytes + kPureDeviceOffsetGpuPending) = 0u;
}

// BOOL D3DDevice_IsFencePending(DWORD Fence) [0x8228F650]: on the device D3D
// finds through VdGlobalDevice; the current fence is kicked first. Games
// poll it, and the fence may be waiting for callbacks only a guest thread can
// run.
u32 IsFencePending_hook(u32 fence) {
  banjo::renut_gpu::CountFenceQuery();  // banjo-nx (package 27c): the stream checks' design
  RunReadyCallbacks();
  if (!fence)
    return 0;
  const u32 device_va = native::GlobalDevice();
  auto *bytes = GuestPtr<u8>(device_va);
  if (!bytes)
    return 0;
  if (fence == *reinterpret_cast<const be_u32 *>(bytes + d3d::kDeviceOffsetFenceTag))
    KickOff_hook(device_va);
  return FencePending(bytes, fence) ? 1 : 0;
}

// BOOL CBlocker::Check() [0x8264DCD0]: one poll of a D3D wait for the GPU
// (ring space, fence markers, streaming). Fence waits no longer come here
// (BlockOnFence_hook); nothing on the host advances the others.
u32 CBlocker_Check_hook(u32 /*blocker*/) {
  return 0;
}

// ---------------------------------------------------------------------------
// Command buffers
// ---------------------------------------------------------------------------

// D3DCommandBuffer fields.
constexpr u32 kBufferFence = 0x08;
constexpr u32 kBufferInheritTags = 0x18;  // D3DTAGCOLLECTION
constexpr u32 kBufferSetTags = 0x40;      // its copy
constexpr u32 kBufferFlags = 0x6C;        // low nibble: state (2 recording, 1 done, 7 out of memory)
constexpr u32 kBufferFixedMemory = 0x98;  // memory the caller gave it, or 0 for its allocator
constexpr u32 kBufferAllocatorSize = 0xA8;

// D3D::ConfigureForOnePassZPass(CDevice*, BOOL, DWORD TileCount, DWORD Width)
// [0x82658818]: the EDRAM layout of a buffer recorded for a one-pass Z pass.
void ConfigureForOnePassZPass(native::Device d, bool flag, u32 tile_count, u32 width) {
  u8 flags3 = d.B(native::off::Flags3) | 0x20;
  if (flag)
    flags3 |= 0x10;
  d.SetB(native::off::Flags3, flags3);
  const u32 msaa = d.H(native::off::SurfaceInfo) & 3;
  u32 tile_height = 8, tile_width = 32, unit = 80;
  if (msaa == 0) {
    tile_height = 16;
  } else if (msaa != 1) {
    tile_width = 16;
    unit = 40;
  }
  const u32 pitch = (tile_width + width - 1) & ~(tile_width - 1);
  const u32 tiles_per_row = pitch / tile_width;
  for (u32 i = 0; i < tile_count; ++i) {
    const u32 x = d.W(native::off::TileOrigins + i * 8);
    const u32 y = d.W(native::off::TileOrigins + i * 8 + 4);
    d.SetW(0x3338 + i * 4, (y / tile_height) * tiles_per_row + x / tile_width);
  }
  d.SetW(0x3374, pitch);
  d.SetW(native::off::SurfaceInfo, native::Insert(d.W(native::off::SurfaceInfo), pitch, 18, 14));
  d.Dirty(2, 0x100);
  d.SetW(0x3378, ((unit + width - 1) / unit) * unit);
  u64 predicated = d.Q(native::off::PredicatedPending2);
  if ((d.B(native::off::Flags3) & 0x20) || (d.W(0x2E4C) & 0xE0000) != 0x40000)
    predicated |= 0x100;
  else
    predicated &= ~u64(0x100);
  d.SetQ(native::off::PredicatedPending2, predicated);
}

// HRESULT D3DDevice_BeginCommandBuffer(D3DDevice*, D3DCommandBuffer*, DWORD Flags,
//     const D3DTAGCOLLECTION *pInheritTags, ..., const D3DRECT *pTilingRects,
//     DWORD TileCount) [0x82657CB0]
// The device records into the buffer: the draws on the host (command_buffers.h),
// so the buffer's segments and the reset of their allocator are left out.
u32 D3DDevice_BeginCommandBuffer_hook(u32 device_va, u32 command_buffer, u32 flags,
                                      u32 inherit_tags, u32 /*unused*/, u32 tiling_rects,
                                      u32 tile_count) {
  // A Begin*Vertices draw recorded into the previous buffer.
  FlushDeferredVertices(device_va);
  native::Device d = native::DeviceAt(device_va);
  auto *buffer = GuestPtr<be_u32>(command_buffer);
  if (!d || !buffer)
    return 0;
  auto bw = [buffer](u32 offset) -> be_u32 & { return buffer[offset / 4]; };
  bw(kBufferFlags) = native::Insert(bw(kBufferFlags), 2, 0, 4);
  if (const u32 fence = bw(kBufferFence)) {
    native::BlockOnFence(native::GlobalDevice(), fence);
    bw(kBufferFence) = 0u;
  }
  d.SetW(native::off::CommandBuffer, command_buffer);
  bw(kBufferFlags) = u32(bw(kBufferFlags)) & ~0x70u;
  for (u32 offset : {0x7Cu, 0x80u, 0x84u, 0x70u, 0x90u, 0x74u, 0x78u})
    bw(offset) = 0u;
  d.SetW(0x3A50, 8);
  d.SetB(native::off::Flags1, d.B(native::off::Flags1) & 0xDF);
  d.SetW(0x3A44, 0);
  d.SetW(0x3A40, bw(kBufferAllocatorSize));
  for (u32 offset = 0x34D0; offset <= 0x34E4; offset += 4)
    d.SetW(offset, 0);
  native::RewindRing(d);

  // The tags the buffer inherits (none set unless given) and its copy.
  auto *tags = GuestPtr<const be_u64>(inherit_tags);
  auto *own = reinterpret_cast<be_u64 *>(reinterpret_cast<u8 *>(buffer) + kBufferInheritTags);
  auto *own_copy = reinterpret_cast<be_u64 *>(reinterpret_cast<u8 *>(buffer) + kBufferSetTags);
  for (u32 i = 0; i < 5; ++i) {
    own[i] = tags ? u64(tags[i]) : 0;
    own_copy[i] = u64(own[i]);
  }
  u32 buffer_flags = bw(kBufferFlags);
  buffer_flags &= ~0x80u;
  buffer_flags &= ~0x8000u;
  buffer_flags |= (((flags << 3) & 0x100) | ((flags >> 4) & 1)) << 7;
  if (flags & 8) {
    d.SetW(native::off::CommandBufferTagMask, ~0u);
    buffer_flags |= 0x400;
  } else {
    d.SetW(native::off::CommandBufferTagMask, 0x400000);
    buffer_flags &= ~0x400u;
  }
  bw(0x78) = 0u;
  buffer_flags &= ~0x21800u;
  bw(kBufferFlags) = buffer_flags;
  d.SetQ(native::off::PredicatedPending2, 0);
  d.SetB(native::off::Flags3, d.B(native::off::Flags3) & 0xCF);
  d.SetB(native::off::Flags, d.B(native::off::Flags) & 0x87);

  if (flags & 3) {
    if (flags & 2) {
      bw(kBufferFlags) = u32(bw(kBufferFlags)) | 0x800;
      d.SetB(native::off::Flags, d.B(native::off::Flags) | 8);
    } else {
      // Its ring allocation list (0x33C0) is the GPU's.
      bw(kBufferFlags) = u32(bw(kBufferFlags)) | 0x1000;
    }
    d.SetW(native::off::TileCount, tile_count);
    d.SetB(native::off::Flags, d.B(native::off::Flags) | 0x30);
    u32 extent_x = 0, extent_y = 0;
    const auto *rects = GuestPtr<const be_u32>(tiling_rects);
    for (u32 i = 0; rects && i < tile_count; ++i) {
      const u32 x1 = rects[i * 4], y1 = rects[i * 4 + 1];
      const u32 x2 = rects[i * 4 + 2], y2 = rects[i * 4 + 3];
      if (extent_x <= x2)
        extent_x = x2;
      if (extent_y <= y2)
        extent_y = y2;
      d.SetW(native::off::TileRects + i * 16, x1);
      d.SetW(native::off::TileRects + i * 16 + 4, y1);
      d.SetW(native::off::TileRects + i * 16 + 8, x2);
      d.SetW(native::off::TileRects + i * 16 + 12, y2);
      d.SetW(native::off::TileOrigins + i * 8, x1 & ~31u);
      d.SetW(native::off::TileOrigins + i * 8 + 4, y1 & ~31u);
    }
    d.SetW(native::off::TilingExtent, extent_x);
    d.SetW(native::off::TilingExtent + 4, extent_y);
    d.SetW(0x3384, 0);
    d.SetW(0x31B0, 2);
    if (flags & 0x40) {
      ConfigureForOnePassZPass(d, (flags >> 7) & 1, tile_count, extent_x);
      bw(kBufferFlags) = u32(bw(kBufferFlags)) | 0x20000;
    }
  }
  if (flags & 4) {
    d.SetB(native::off::Flags, d.B(native::off::Flags) | 0x40);
    d.SetW(0x31A0, native::Extract(d.W(native::off::DepthControl), 4, 3));
    if (d.B(native::off::Flags3) & 0x20) {
      u64 predicated = d.Q(native::off::PredicatedPending2) | (u64(1) << 37);
      if (d.W(0x3378) != (d.W(native::off::SurfaceInfo) & 0x3FFF))
        predicated |= u64(1) << 57;
      d.SetQ(native::off::PredicatedPending2, predicated);
    }
  }
  d.SetW(native::off::PredicationSet, 0);
  native::UpdateTilePredicated(d);
  d.SetW(native::off::Predication, ~0u);
  d.SetW(native::off::RunPredicationState, 0);
  d.SetW(native::off::TilePredication, 0);
  d.SetW(native::off::RunPredication, 0);
  // The buffer starts with no fetch constants, and dirty in everything it does
  // not inherit.
  std::memset(d.bytes + native::off::TextureFetch, 0, 0x300);
  for (u32 i = 0; i < 5; ++i)
    d.SetPending(i, ~u64(own[i]));

  BeginRecording(device_va, command_buffer);
  return 0;
}

// HRESULT D3DDevice_EndCommandBuffer(D3DDevice*) [0x826582A0]
// The state left dirty would be written into the buffer (Flags bit 5 at
// Begin); then everything is unbound, and the device is pointed at its side
// buffer until the next Begin.
u32 D3DDevice_EndCommandBuffer_hook(u32 device_va) {
  FlushDeferredVertices(device_va);
  native::Device d = native::DeviceAt(device_va);
  if (!d)
    return 0;
  auto *buffer = GuestPtr<be_u32>(d.W(native::off::CommandBuffer));
  if (buffer && (u32(buffer[kBufferFlags / 4]) & 0x8000)) {
    for (u32 i = 0; i < 5; ++i)
      d.SetPending(i, 0);
  }
  native::UnsetAll(d);
  // A buffer in fixed memory reports running out of it here; recorded on the
  // host, it does not.
  if (buffer) {
    if (u32(buffer[kBufferFlags / 4]) & 0x1000)
      buffer[0x78 / 4] = 0u;  // its ring allocation list: none
  }
  d.SetW(native::off::CommandBuffer, 0);
  if (buffer) {
    const u32 end = d.W(native::off::Ring) + 4;
    buffer[0x88 / 4] = end;
    buffer[0x8C / 4] = d.W(native::off::RingLimit);
    buffer[0x90 / 4] = native::GpuAddress(end) - 0x40000000u;
  }
  const bool out_of_memory = (d.B(native::off::Flags1) & 0x20) != 0;
  if (buffer)
    buffer[kBufferFlags / 4] = native::Insert(buffer[kBufferFlags / 4], out_of_memory ? 7 : 1, 0, 4);
  // CDevice::MarkAsOutOfMemory: the device writes nowhere until the next Begin.
  native::RewindRing(d);
  d.SetB(native::off::Flags1, d.B(native::off::Flags1) | 0x20);
  return out_of_memory ? kE_OUTOFMEMORY : 0;
}

// void D3DDevice_RunCommandBuffer(D3DDevice*, D3DCommandBuffer*,
//     DWORD PredicationSelect) [0x82222888]
// Replaces the original, which would send the buffer's packets to the GPU
// (and on a no-hardware device append to a buffer being recorded).
void D3DDevice_RunCommandBuffer_hook(u32 device_va, u32 command_buffer,
                                     u32 /*predication_select*/) {
  if (!command_buffer)
    return;
  if (IsRecordingCommandBuffer(device_va)) {
    RecordRun(device_va, command_buffer);
  } else {
    RunReadyCallbacks();
    QueueRunCommandBuffer(device_va, command_buffer);
  }
}

// void D3DDevice_Swap(D3DDevice *pDevice, D3DBaseTexture *pFrontBuffer,
//     const D3DVIDEO_SCALER_PARAMETERS *pParameters) [0x82287590]
// The frame goes to the render thread, which presents it; the game thread
// goes on with the next one, waiting only when it is more than
// render_thread::kMaxFramesAhead ahead.
constexpr u32 kSa2DeviceOffsetGammaRamp = 15004;  // pure-nx: N&B's +0x3A9C (sa2-nx: SA2's 15408, the +404 block)

// sa2-nx (2026-10-20): the SDK's core layout (system/xthread.cpp).
REXCVAR_DEFINE_BOOL(sa2_renut_core_layout, true, "SA2/Renderer",
                    "Renut's renderer on the Switch: at the first Swap the guest main thread (SA2's game and drawing "
                    "thread) is pinned to core 0 and every other guest thread gets cores 1-2 (the render thread is on "
                    "core 1), so no game thread time-slices with it. Off: every guest thread on any core, as before")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// pure-nx (2026-11-07, pure_004's rex_profile and guest profile): in races the game's FMOD mixer thread (start
// 82200570 -> 82D6E198, 45% of a core) shared core 2 in 10 ms slices with the game's ten job workers (8296A690) the
// main thread waits for (829CD948: 14-19% of the update, 11-12% of the end of the frame) and the main thread waited
// for the mixer's FMOD lock (RtlEnterCriticalSection from 82D6D6C8: 4-8% of the update). Moved to core 1 alone with
// the render thread (30%, at 0x2C above it), core 2 is the workers'. A comma-separated list of start:preferred:mask
// (hex start, decimal core, hex mask); empty: none.
// pure-nx (2026-11-08, pure_008: the sound fell behind, slowed down and distorted): on core 1 the mixer had the
// render thread (0x2C, 29%) and the XMA Decoder (0x2B, 25%) above it and starved (40% of the core left over, mask 0x2:
// nowhere else to go). Default empty again: the mixer with the other guest threads on cores 1-2 (preferred 2), as when
// the sound was right; the setting stays for experiments.
// pure-nx (2026-11-08, pure_011: the first run with it on ended in a hang at the end of a race (the main thread
// blocked for good in the game's WaitForSingleObject, every job worker idle) and the race was no faster (13-16 fps, as
// pure_008/009): off again by default while the hang is looked into (pure_frame_probe's wait watchdog).
REXCVAR_DEFINE_BOOL(pure_share_main_core, false, "Pure",
                    "The other guest threads may use core 0 while the main thread waits (the main thread at 0x3A, one "
                    "above them, takes it back at once); off: cores 1-2 only, as sa2_renut_core_layout")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
extern "C" void RexSwitchSetCurrentThreadPriority(int priority);

// pure-nx (2026-11-08): with four cores (renut-nx's four-core mode, tools/rexglue-sdk-pure core/threading_switch.cpp)
// the game's FMOD threads (start 82200570: the mixer took 40-45% of a core in races, on cores 1-2 with the ten job
// workers, the XMA Decoder and the render thread) go to the system core alone, as renut-nx's XAudio mixer.
REXCVAR_DEFINE_BOOL(pure_system_core_fmod, true, "Pure",
                    "Four-core mode: the game's FMOD threads (the mixer, start 82200570) on the system core (core 3) "
                    "alone; no effect with three cores or when pure_guest_thread_cores is set")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// pure-nx (2026-11-08, pure_014: four cores, races at 17-22 fps, the main thread waiting 7-9 ms a frame for the game's
// ten job workers (82969D30 / 8296A4CC) while cores 1-2 were about half idle on average but full in the job bursts,
// next to the render thread (0x2C) and the XMA Decoder (0x2B), both above the workers): the workers on core 3 too, and
// the FMOD threads above them there, so the mixer keeps its core whenever it wants it (Horizon time-slices only 0x3B).
REXCVAR_DEFINE_BOOL(pure_system_core_jobs, true, "Pure",
                    "Four-core mode: the game's job workers (start 8296A690) on cores 1-3 instead of 1-2; no effect "
                    "with three cores or when pure_guest_thread_cores is set")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(pure_system_core_fmod_priority, 0x2D, "Pure",
                     "Four-core mode: Horizon priority of the game's FMOD threads on the system core (0x2D: above the "
                     "job workers' 0x3B there; -1 = unchanged)")
    .range(-1, 0x3F)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// pure-nx (2026-11-08, pure_015/016: the main thread still waited ~6.3 ms a frame for the job workers, which run
// their jobs at 0x3B, time-sliced among themselves and with every other guest thread): the workers above the guest
// threads (0x2D, still below the render thread's 0x2C and the audio's 0x2B), so a job runs to its end once started.
// They block on the game's condition variable between jobs. -1 = unchanged (0x3B), the way back if anything stalls.
REXCVAR_DEFINE_INT32(pure_job_worker_priority, 0x2D, "Pure",
                     "Horizon priority of the game's job workers (start 8296A690); -1 = unchanged (0x3B)")
    .range(-1, 0x3F)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
extern "C" int RexSwitchSystemCore(void);
extern "C" bool RexSetGuestThreadCoreOverrideEx(uint32_t start_address, int preferred, uint32_t mask, int priority);

REXCVAR_DEFINE_STRING(pure_guest_thread_cores, "", "Pure",
                      "Cores for the guest threads of given start routines, over sa2_renut_core_layout: "
                      "start:preferred:mask, comma-separated (82200570:1:2: the FMOD mixer on core 1)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

void D3DDevice_Swap_hook(u32 device_va, u32 front_buffer, u32 /*parameters*/) {
  // sa2-nx (2026-10-20, sa2_renut_core_layout, the Switch): once, the guest main thread (SA2's game and drawing thread,
  // about a core of work in City Escape) alone on core 0, the other guest threads on cores 1-2 (preferring 2: the
  // render thread is pinned to core 1 at a higher priority). Before, every guest thread could run on any core, and at
  // their priority (0x3B) Horizon time-slices the threads of a core in 10 ms slices.
  static bool core_layout_done = false;
  if (!core_layout_done) {
    core_layout_done = true;
#if REX_PLATFORM_SWITCH
    if (REXCVAR_GET(sa2_renut_core_layout)) {
      // pure-nx: the overrides first (pure_guest_thread_cores).
      const std::string overrides = REXCVAR_GET(pure_guest_thread_cores);
      size_t pos = 0;
      while (pos < overrides.size()) {
        size_t end = overrides.find(',', pos);
        if (end == std::string::npos) end = overrides.size();
        const std::string item = overrides.substr(pos, end - pos);
        unsigned start = 0, mask = 0;
        int preferred = -1;
        if (std::sscanf(item.c_str(), " %x:%d:%x", &start, &preferred, &mask) == 3 && mask &&
            RexSetGuestThreadCoreOverride(start, preferred, mask)) {
          RGPU_INFO("[pure] guest threads of {:08X} on core {} (mask {:X}): pure_guest_thread_cores", start, preferred,
                    mask);
        } else if (!item.empty()) {
          RGPU_WARN("[pure] pure_guest_thread_cores: '{}' not understood (start:preferred:mask)", item);
        }
        pos = end + 1;
      }
      // pure-nx (2026-11-08): four cores: the FMOD threads on the system core (pure_system_core_fmod).
      const int system_core = RexSwitchSystemCore();
      if (overrides.empty() && system_core >= 0 && REXCVAR_GET(pure_system_core_fmod)) {
        const int priority = REXCVAR_GET(pure_system_core_fmod_priority);
        if (RexSetGuestThreadCoreOverrideEx(0x82200570u, system_core, 1u << system_core, priority))
          RGPU_INFO("[pure] the game's FMOD threads (82200570) on the system core {} at priority {} "
                    "(pure_system_core_fmod, pure_system_core_fmod_priority)",
                    system_core, priority < 0 ? std::string("unchanged") : fmt::format("0x{:X}", priority));
      }
      // pure-nx (2026-11-08): the job workers on cores 1-3 (pure_system_core_jobs).
      if (overrides.empty()) {
        const bool wide = system_core >= 0 && REXCVAR_GET(pure_system_core_jobs);
        const uint32_t mask = wide ? (0x6u | (1u << system_core)) : 0x6u;
        const int priority = REXCVAR_GET(pure_job_worker_priority);
        if ((wide || priority >= 0) && RexSetGuestThreadCoreOverrideEx(0x8296A690u, 2, mask, priority))
          RGPU_INFO("[pure] the game's job workers (8296A690) on cores mask 0x{:X} at priority {} "
                    "(pure_system_core_jobs, pure_job_worker_priority)",
                    mask, priority < 0 ? std::string("unchanged") : fmt::format("0x{:X}", priority));
      }
      // pure-nx (2026-11-08, pure_009's guest profile: the main thread blocked 14.5% of the update and 11% of the end
      // of the frame waiting for the game's ten job workers, which shared core 2 with the FMOD mixer while core 0 sat
      // idle in those waits): pure_share_main_core lets the other guest threads use core 0 too, and puts the main
      // thread one priority above them (0x3A), so on core 0 they run only while the main thread waits and give the
      // core back the moment it is ready (Horizon preempts a lower priority at once). A main thread that does not
      // block keeps core 0 to itself, as before.
      const bool share = REXCVAR_GET(pure_share_main_core);
      const uint32_t moved = RexSetGuestThreadCoreLayout(0, share ? 0x7 : 0x6, 2);
      if (share)
        RexSwitchSetCurrentThreadPriority(0x3A);
      RGPU_INFO("[renut] sa2_renut_core_layout: the guest main thread on core 0{}, the other guest threads on cores "
                "{} ({} threads set)", share ? " at 0x3A" : "", share ? "0-2 (pure_share_main_core)" : "1-2", moved);
    }
#endif
  }
  // sa2-nx: the console's vertical blank paced SA2's frames (its game speed follows them): sa2_renut_pacing.cpp.
  sa2::renut::PaceSwap(device_va);
  // sa2-nx: SA2's game frame count (src/sa2_d3d_trace.cpp: the flip, which only the GPU's vblank interrupt reaches).
  const uint64_t game_frame = g_sa2_game_frames.fetch_add(1, std::memory_order_relaxed) + 1;
  // sa2-nx (1.0.3.1): the PC's docked / handheld test toggles (sa2_render_test_toggle_at; nothing on the Switch).
  sa2::render::OnPresented(game_frame);
  // sa2-nx: a lockstep recording (sa2_capture.cpp) takes this very frame: asked before it is queued.
  const uint64_t capture_ticket = sa2::capture::RenutLockstepOn() ? RequestFrameCapture() : 0;
  renut::frame_stats::OnGameFrame();
  rex::latency::NoteGuestSwap();  // banjo-nx (2026-10-19, input latency): the probe
  const uint64_t swap_ordinal = rex::latency::SwapCount();
  FlushDeferredVertices(device_va);
  SnapshotDeltasNewFrame();  // sa2-nx (2026-10-20, sa2_renut_snapshot_deltas)

  // What the original does after handing the frame to the video hardware:
  // count the swap, record the frame's fence tag, and end the frame with a
  // KickOff, whose fence completes once the render thread gets there.
  auto *device = GuestPtr<u8>(device_va);
  if (device) {
    auto word = [device](u32 offset) -> be_u32 & {
      return *reinterpret_cast<be_u32 *>(device + offset);
    };
    word(d3d::kDeviceOffsetSwapCount) = u32(word(d3d::kDeviceOffsetSwapCount)) + 1;
    const u32 fence_tag = word(d3d::kDeviceOffsetFenceTag);
    word(d3d::kDeviceOffsetSwapFenceTag) = fence_tag;
    word(d3d::kDeviceOffsetPresentFenceTag) = fence_tag;
    device[d3d::kDeviceOffsetSwapFlags] &= u8(~d3d::kSwapFlagPending);
    // sa2-nx (2026-10-20): the display gamma ramp SA2's SetGammaRamp keeps in the device (sub_825527D8: +15408, 768
    // words; N&B's +0x3A9C), which the original Swap writes to the display controller's table, goes to the
    // scan-out (backend/present.cpp, sa2_renut_gamma_ramp).
    if (REXCVAR_GET(sa2_renut_gamma_ramp))
      Video::SetGammaRamp(device + kSa2DeviceOffsetGammaRamp);
    // banjo-nx (package 27c): the frame's last vertex and texture fills are read here, before the Swap's fence
    // completes. They were read at the present, after it: the game, told the frame was done, could rewrite a dynamic
    // buffer for the next frame first, and the frame's last draws (the composite, the HUD) took the new bytes.
    banjo::renut_gpu::checks::QueueKickFlush();
    KickFence(device);
    // sa2-nx: banjo-nx's banjo_renut_game_gamma (N&B's +0x3A9C) is not taken; SA2's ramp goes to the present above
    // (sa2_renut_gamma_ramp).
  }

  render_thread::QueueFrame([front_buffer, swap_ordinal] {
    PresentImage image;
    {
      std::lock_guard lock(State().mutex);
      image = FrontBufferImageLocked(front_buffer);
      if (image.texture)
        State().last_front_buffer = front_buffer;
      AdvanceTextureFrameLocked();
      AdvanceStreamCacheFrameLocked();
      LatchResolutionScaleLocked();
    }
    MaybeReportMemory();
    // The front buffer is the texture the game resolved the frame into.
    Video::NotePresentingSwap(swap_ordinal);  // banjo-nx (2026-10-20): the probe's "input -> GPU done"
    Video::Present(image.texture ? &image : nullptr);
    {
      std::lock_guard lock(State().mutex);
      ::renut::gpu::SyncPipelineListLocked();  // banjo-nx (2026-10-20): the list's new records to the card
    }
    rex::latency::NotePresented(swap_ordinal);  // banjo-nx (2026-10-19, input latency)
    banjo::display_sync::NotePresented(swap_ordinal);  // banjo-nx (2026-10-19, latency batch 3)
  });
  RunReadyCallbacks();
  if (capture_ticket) {
    rex::ui::RawImage image;
    const bool ok = WaitFrameCapture(capture_ticket, image, 2000);
    sa2::capture::RenutLockstepFrame(game_frame, ok ? &image : nullptr);
  }

  // sa2-nx: no ResetAllState (N&B's dirty masks; nothing reads them here: the draws read the device's state, and
  // SA2's own setters keep marking it).
  if (device)
    native::RewindRing(native::DeviceAt(device_va));
}

// void D3DDevice_InsertCallback(D3DDevice*, D3DCALLBACKTYPE, D3DCALLBACK pCallback,
//                               DWORD Context) [0x821FB688]
// The GPU would call pCallback(Context) on reaching this point in the ring,
// and the caller may reuse whatever the preceding work reads once it runs: it
// runs once the render thread has read that work's data (callbacks.h). N&B's
// callers: EndTiling (D3D's WorkerDpc) and the XMV renderer
// (CCalVideoRenderer::Render 0x82A0D9E8, which counts frames in flight until
// IXMediaXmvPlayer_Resume and frees itself after the last).
void D3DDevice_InsertCallback_hook(u32 device, u32 /*type*/, u32 callback, u32 context) {
  if (!callback)
    return;
  FlushDeferredVertices(device);
  RunReadyCallbacks();
  InsertCallback(callback, context);
}

// ---------------------------------------------------------------------------
// Device queries, thread ownership, presentation state
// ---------------------------------------------------------------------------

constexpr u32 kDeviceOffsetReferenceCount = 0x3C;
constexpr u32 kDeviceOffsetOwnerThread = 0x2A88;
constexpr u32 kDeviceCapsVa = 0x82183B00;  // N&B's constant D3DCAPS9
constexpr u32 kDeviceCapsSize = 0x130;
constexpr u32 kFusionVa = 0x82F597B4;      // non-zero on a Fusion GPU

// ULONG D3DDevice_AddRef(D3DDevice*) [0x8264E068]
u32 D3DDevice_AddRef_hook(u32 device_va) {
  native::Device d = native::DeviceAt(device_va);
  if (!d)
    return 0;
  const u32 count = d.W(kDeviceOffsetReferenceCount) + 1;
  d.SetW(kDeviceOffsetReferenceCount, count);
  return count;
}

// void D3DDevice_AcquireThreadOwnership(D3DDevice*) [0x8264E0D8]
void D3DDevice_AcquireThreadOwnership_hook(u32 device_va) {
  native::Device d = native::DeviceAt(device_va);
  if (d)
    d.SetW(kDeviceOffsetOwnerThread, GuestGetCurrentThreadId());
}

// void D3DDevice_ReleaseThreadOwnership(D3DDevice*) [0x8264E118]
void D3DDevice_ReleaseThreadOwnership_hook(u32 device_va) {
  native::Device d = native::DeviceAt(device_va);
  std::atomic_thread_fence(std::memory_order_seq_cst);  // eieio
  if (d)
    d.SetW(kDeviceOffsetOwnerThread, 0);
}

// void D3DDevice_GetDeviceCaps(D3DDevice*, D3DCAPS9*) [0x82658948]
void D3DDevice_GetDeviceCaps_hook(u32 /*device*/, u32 caps_va) {
  auto *caps = GuestPtr<u8>(caps_va);
  const auto *source = GuestPtr<const u8>(kDeviceCapsVa);
  if (caps && source)
    std::memcpy(caps, source, kDeviceCapsSize);
}

// void D3DDevice_GetDisplayMode(D3DDevice*, UINT iSwapChain, D3DDISPLAYMODE*)
// [0x82658960]: width, height, refresh rate, format.
void D3DDevice_GetDisplayMode_hook(u32 device_va, u32 /*swap_chain*/, mapped_u32 mode) {
  const native::Device d = native::DeviceAt(device_va);
  if (!d || !mode)
    return;
  mode[0] = d.W(0x34E8);
  mode[1] = d.W(0x34EC);
  mode[2] = d.W(0x5434);
  mode[3] = d.W(0x34F0);
}

// void D3DDevice_BlockOnFence(DWORD Fence) [0x8228F3B0]: on the device D3D
// finds through VdGlobalDevice.
void D3DDevice_BlockOnFence_hook(u32 fence) {
  if (fence)
    native::BlockOnFence(native::GlobalDevice(), fence);
}

// void D3D::SynchronizeToPresentationInterval(CDevice*, BOOL) [0x8228A748]:
// its swap callback (SwapCallback) and the waits around it go into the ring;
// the swap is then pending.
void D3D_SynchronizeToPresentationInterval_hook(u32 device_va, u32 /*immediate*/) {
  native::Device d = native::DeviceAt(device_va);
  if (!d)
    return;
  native::SetPredication(d, 0);
  d.SetB(native::off::Flags1, d.B(native::off::Flags1) | d3d::kSwapFlagPending);
}

// void D3DResource_GetDevice(D3DResource*, D3DDevice**) [0x82654D50]: the
// device D3D finds through VdGlobalDevice, referenced.
void D3DResource_GetDevice_hook(u32 /*resource*/, mapped_u32 out_device) {
  const u32 device = native::GlobalDevice();
  if (device)
    D3DDevice_AddRef_hook(device);
  if (out_device)
    *out_device = device;
}

// BOOL D3D::IsFusion() [0x82B6BF60]
u32 D3D_IsFusion_hook() {
  return LoadGuestU32(kFusionVa) != 0 ? 1 : 0;
}

// D3D's gamma tables (sub_82659140, _Gamma_sRGB 0x82659090): 1024 entries
// taking a 10-bit value through the display's gamma (VdGetCurrentDisplayGamma).
// Inverse goes from the display's curve to linear. D3D caches the table per
// display gamma (unk_82FB2558); it is computed per call here.
float Fmsubs(float a, float b, float c) {
  return std::fmaf(a, b, -c);
}

float Bits(u32 bits) {
  return std::bit_cast<float>(bits);
}

float GammaSRGB(float v, bool inverse) {
  if (inverse) {
    return v > 0.04045 ? float(std::pow(double((v + Bits(0x3D6147AE)) * Bits(0x3F72A76F)), 2.4000000953674316))
                       : v * Bits(0x3D9E8391);
  }
  return v > 0.0031308 ? Fmsubs(float(std::pow(double(v), 0.4166666567325592)), Bits(0x3F870A3D),
                                Bits(0x3D6147AE))
                       : v * Bits(0x414EB852);
}

std::array<u16, 1024> GammaTable(bool inverse) {
  // Guest words for the kernel's outputs.
  static const u32 scratch = [] {
    void *host = records::AllocRaw(8);
    return host ? REX_KERNEL_MEMORY()->HostToGuestVirtual(host) : 0u;
  }();
  u32 type = 0;
  float gamma = 0.0f;
  if (scratch) {
    VdGetCurrentDisplayGamma(scratch, scratch + 4);
    type = LoadGuestU32(scratch);
    gamma = std::bit_cast<float>(LoadGuestU32(scratch + 4));
  }
  std::array<u16, 1024> table{};
  for (u32 i = 0; i < 1024; ++i) {
    float v = float(i) * Bits(0x3A802008);  // flt_8219F708, 1 / 1023
    if (!inverse) {
      v = v > 0.04045 ? float(std::pow(double((v + Bits(0x3D6147AE)) * Bits(0x3F72A76F)), 2.4000000953674316))
                      : v * Bits(0x3D9E8391);
    }
    float out;
    if (type == 1) {
      out = GammaSRGB(v, inverse);
    } else if (type == 3) {
      out = float(std::pow(double(v), double(inverse ? gamma : 1.0f / gamma)));
    } else if (inverse) {
      out = v < 0.0812 ? v * Bits(0x3E638E39)
                       : float(std::pow(double((v + Bits(0x3DCAC083)) * Bits(0x3F68F065)), 2.222222328186035));
    } else {
      out = v < 0.018 ? v * 4.5f
                      : Fmsubs(float(std::pow(double(v), 0.44999998807907104)), Bits(0x3F8CAC08),
                               Bits(0x3DCAC083));
    }
    if (inverse) {
      out = out > 0.0031308 ? Fmsubs(float(std::pow(double(out), 0.4166666567325592)), Bits(0x3F870A3D),
                                     Bits(0x3D6147AE))
                            : out * Bits(0x414EB852);
    }
    const i32 value = native::TruncateToI32(std::fma(double(out), 1023.0, 0.5));
    table[i] = u16(std::clamp(value, 0, 0x3FF));
  }
  return table;
}

// The device's gamma (+0x3A9C, red, green, blue: 256 words each) for the
// display to pick up; the renderer presents without it.
constexpr u32 kDeviceOffsetGammaRamp = 0x3A9C;

void StoreGamma(native::Device d, const be_u16 (&corrected)[768]) {
  if (std::memcmp(corrected, d.bytes + kDeviceOffsetGammaRamp, sizeof(corrected)) == 0)
    return;
  if (KeGetCurrentProcessType() != 2) {
    // The display's pending copy takes the previous ramp first.
    const u32 pending = d.W(0x5D90);
    if (pending && !(d.W(0x5D94) & 0x80000000u)) {
      if (auto *dest = GuestPtr<u8>(pending))
        std::memcpy(dest, d.bytes + kDeviceOffsetGammaRamp, sizeof(corrected));
    }
    d.SetW(0x5D94, d.W(0x5D94) | 0x80000000u);
  }
  std::memcpy(d.bytes + kDeviceOffsetGammaRamp, corrected, sizeof(corrected));
  d.SetW(0x4144, d.W(0x4144) | 2);
}

// D3DDevice_SetGammaRamp's work: the ramp (D3DGAMMARAMP, 3 x 256 words)
// through the gamma table (GammaRampCorrected 0x826593F8).
void SetGammaRamp(native::Device d, const u16 *ramp) {
  const std::array<u16, 1024> table = GammaTable(false);
  be_u16 corrected[768];
  for (u32 i = 0; i < 768; ++i)
    corrected[i] = u16((table[ramp[i] >> 6] & 0x3FF) << 6);
  StoreGamma(d, corrected);
}

// D3DDevice_SetPWLGamma's: D3DPWLGAMMA (3 x 128 {base, delta}) through the
// table (sub_82659490), each segment's end clamped to 0x3FF.
void SetPWLGamma(native::Device d, const u16 *pwl) {
  const std::array<u16, 1024> table = GammaTable(false);
  be_u16 corrected[768];
  for (u32 c = 0; c < 3; ++c) {
    for (u32 i = 0; i < 128; ++i) {
      const u32 at = c * 256 + i * 2;
      const u32 base = pwl[at] >> 6;
      const u32 end = std::min(base + (pwl[at + 1] >> 6), 0x3FFu);
      corrected[at] = u16((table[base] & 0x3FF) << 6);
      corrected[at + 1] = u16(((table[end] - table[base]) & 0x3FF) << 6);
    }
  }
  StoreGamma(d, corrected);
}

void ReadRamp(u32 ramp_va, u16 (&ramp)[768]) {
  const auto *guest = GuestPtr<const be_u16>(ramp_va);
  for (u32 i = 0; i < 768; ++i)
    ramp[i] = guest ? u16(guest[i]) : 0;
}

// void D3DDevice_SetGammaRamp(D3DDevice*, DWORD Flags, const D3DGAMMARAMP*)
// [0x8264D948]
void D3DDevice_SetGammaRamp_hook(u32 device_va, u32 /*flags*/, u32 ramp_va) {
  native::Device d = native::DeviceAt(device_va);
  if (!d || !ramp_va)
    return;
  u16 ramp[768];
  ReadRamp(ramp_va, ramp);
  SetGammaRamp(d, ramp);
}

// void D3DDevice_SetPWLGamma(D3DDevice*, DWORD Flags, const D3DPWLGAMMA*)
// [0x8264DA18]
void D3DDevice_SetPWLGamma_hook(u32 device_va, u32 /*flags*/, u32 pwl_va) {
  native::Device d = native::DeviceAt(device_va);
  if (!d || !pwl_va)
    return;
  u16 pwl[768];
  ReadRamp(pwl_va, pwl);
  SetPWLGamma(d, pwl);
}

// The D3D internals Direct3D_CreateDevice and D3DDevice_Release run, for any
// other caller.
void D3D_XenonGpu_Initialize_hook(u32 device) {
  XenonGpuInitialize(native::DeviceAt(device));
}
void D3D_InitializeApiState_hook(u32 device) {
  InitializeApiState(native::DeviceAt(device));
}
u32 D3D_InitializePresentationParameters_hook(u32 device, u32 parameters) {
  return InitializePresentationParameters(native::DeviceAt(device), parameters) ? 1 : 0;
}
void D3D_CDevice_Uninitialize_hook(u32 device) {
  UninitializeDevice(native::DeviceAt(device));
}

} // namespace

namespace renut::gpu::native {

void RewindRing(Device device) {
  if (!device || !g_ring_sink)
    return;
  device.SetW(off::Ring, g_ring_sink);
  device.SetW(off::RingLimit, g_ring_sink + kRingSinkSize);
  device.SetW(off::RingGuarantee, g_ring_sink + kRingSinkSize - kRingSinkGuaranteeMargin);
}

u32 KickOff(u32 device_va) {
  return KickOff_hook(device_va);
}

void BlockOnFence(u32 device_va, u32 fence) {
  BlockOnFence_hook(device_va, fence, 0, 0);
}

u32 GlobalDevice() {
  const u32 global = LoadGuestU32(kVdGlobalDeviceImportSlot);
  return global ? LoadGuestU32(global) : 0;
}

void Swap(u32 device_va, u32 front_buffer, u32 parameters) {
  D3DDevice_Swap_hook(device_va, front_buffer, parameters);
}

// CDevice::SetFence (0x82278528): with immediate fences the device writes the
// completion itself (unless +0x541C is set); otherwise the fence completes on
// the render thread, as a KickOff's. A device recording a command buffer
// would put it into the buffer, which runs on the host.
void SetFence(u32 device_va) {
  auto *bytes = GuestPtr<u8>(device_va);
  if (!bytes)
    return;
  if (bytes[kDeviceOffsetFlags2] & kDeviceFlagImmediateFences) {
    Device d = DeviceAt(device_va);
    const u32 fence = d.W(off::FenceTag);
    if (!d.W(0x541C)) {
      if (auto *completion = GuestPtr<be_u32>(d.W(off::FenceCompletion))) {
        completion[0] = fence;
        completion[1] = (d.W(0x3A48) & 3) | d.W(off::Ring);
      }
    }
    d.SetW(off::FenceTag, fence + 2);
    return;
  }
  if (!IsRecordingCommandBuffer(device_va))
    KickDevice(GuestPtr<d3d::Device>(device_va), device_va);
}

} // namespace renut::gpu::native


// ---------------------------------------------------------------------------
// pure-nx (2026-11-06): Pure's hooks (Dishonored's addresses, then SA2's or N&B's, in brackets)
// ---------------------------------------------------------------------------
// Pure links Direct3D 2.0.6995: the private device layout is N&B's first release (d3d.h), a 0x5E00-byte device made by
// Direct3D_CreateDevice 0x829CFB38 (the constructor 0x82A043B8: the creation thread, the side buffer; the behaviour
// flags at 23932), which then calls the hardware path 0x82A04440 for a title device. The hook is on that hardware path,
// as dishonored-nx's: it does everything Pure's does but the GPU's work and marks the device "no hardware" as SA2's
// type-2 path does. Pure's hardware path, step by step (tools/census, the disassembly of 0x82A04440):
//   RtlInitializeCriticalSection(+14944, +14972); +10942 |= 4
//   0x82A04308: VdInitializeEngines, two title-terminate notifications, +16712 = 0, VdSetGraphicsInterruptCallback
//   *VdGlobalDevice (the slot at 0x83850B84) = device; ExGetXConfigSetting(3, 10) into +16716
//   0x829EA750: D3D::InitializeRingBuffer (the completed-fence word +10896, the scratch word +10900); 0 is success
//   0x829CEFB0, 0x82A08028 (the interrupt DPC and its thread), 0x829DC510: each non-zero on success
//   +21560 = +21564 = -1; 0x82A0A508 (the initial state packets); 0x82A03DB0 (InitializePresentationParameters,
//   non-zero on success); 0x82A03AA8 (InitializeApiState)
//   the kernel's graphics statistics (slot 0x83850AF4): +16720, +16724, +16560 (the swap count), +21576
//   KickOff 0x829EA048; QueryPerformanceFrequency into +21588, its reciprocal +21592
//   0x82A0E098; VdRetrainEDRAM 0x82A13528; 0x829D4160 (device, 0: the initial viewport and scissor);
//   VdIsHSIOTrainingSucceeded and the EDRAM check 0x82A13830 (messages only)
// Pure's Direct3D is not LTCG'd as Dishonored's: the draws, ClearF and Resolve take N&B's arguments.

REX_EXTERN(__imp__sub_829CEFB0);  // device setup, memory only [dishonored-nx 82A52488]
REX_EXTERN(__imp__sub_829DC510);  // device setup, memory only [dishonored-nx 82A48150]
REX_EXTERN(__imp__sub_82A0A508);  // initial state packets (ring sink), KickOff [dishonored-nx 82A645A8]
REX_EXTERN(__imp__sub_82A03DB0);  // D3D::InitializePresentationParameters (CDevice*, D3DPRESENT_PARAMETERS*) [82A5F168]
REX_EXTERN(__imp__sub_82A03AA8);  // D3D::InitializeApiState (CDevice*) [dishonored-nx 82A5EE88]
REX_EXTERN(__imp__sub_82A0E098);  // device setup, memory only [dishonored-nx 82A62FB8]
REX_EXTERN(__imp__sub_829D4160);  // initial viewport / scissor (device, 0) [dishonored-nx 82A448E0]

REXCVAR_DEFINE_INT32(pure_renut_query_samples, 1000, "Pure/Renderer",
                     "Renut: the samples each occlusion query reports as passed (every query visible)");

namespace {

void CallGuest(PPCContext &ctx, uint8_t *base, void (*function)(PPCContext &, uint8_t *), u32 r3, u32 r4 = 0,
               u32 r5 = 0) {
  ctx.r3.u64 = r3;
  ctx.r4.u64 = r4;
  ctx.r5.u64 = r5;
  function(ctx, base);
}

// BOOL D3D::InitializeHardwareDevice(CDevice*, D3DPRESENT_PARAMETERS*) [0x82A04440; dishonored-nx sub_82A5F9A0; SA2
// sub_8255CDA0]: the device as the hardware path leaves it, without a GPU. 1 on success, 0 on failure (the caller
// frees the device).
void PureInitializeDevice(PPCContext &ctx, uint8_t *base) {
  const u32 device_va = ctx.r3.u32;
  const u32 present_parameters = ctx.r4.u32;
  native::Device d = native::DeviceAt(device_va);
  auto fail = [&ctx] { ctx.r3.u64 = 0; };
  if (!d || !records::Init())
    return fail();
  if (!g_ring_sink)
    g_ring_sink = records::AllocPhysicalBlock(kRingSinkSize);
  if (!g_ring_data)
    g_ring_data = records::AllocPhysicalBlock(kRingDataSize);
  const u32 fence_va = records::AllocPhysicalBlock(0x80 + 0x20);
  if (!g_ring_sink || !g_ring_data || !fence_va) {
    RGPU_ERROR("Pure device: guest allocation failed");
    return fail();
  }
  std::memset(GuestPtr<u8>(g_ring_sink), 0, kRingSinkSize);
  std::memset(GuestPtr<u8>(fence_va), 0, 0x80 + 0x20);
  u8 *bytes = d.bytes;

  // The hardware path's order (0x82A04440), the GPU's steps left out.
  RtlInitializeCriticalSection(device_va + kDeviceOffsetCriticalSection0);
  RtlInitializeCriticalSection(device_va + kDeviceOffsetCriticalSection1);
  d.SetB(10942, d.B(10942) | 4);
  // (0x82A04308: VdInitializeEngines, the title-terminate notifications, VdSetGraphicsInterruptCallback: the GPU's.)
  d.SetW(16712, 0);
  if (const u32 global = LoadGuestU32(kVdGlobalDeviceImportSlot))
    *GuestPtr<be_u32>(global) = device_va;
  // (ExGetXConfigSetting(3, 10) into +16716 and D3D::InitializeRingBuffer (0x829EA750): the GPU's ring; the sink
  // instead, with the completed-fence word (128 bytes in the original) and the swap's scratch word (32). As SA2's
  // type-2 device: no hardware, fences completing as they are written until the device is set up.)
  WriteU32(bytes, kDeviceOffsetFenceCompletion, fence_va);
  WriteU32(bytes, 10900, fence_va + 0x80);
  bytes[kDeviceOffsetFlags] |= kDeviceFlagNoHardware;
  bytes[kDeviceOffsetFlags2] &= u8(~0x20);
  bytes[kDeviceOffsetFlags2] |= kDeviceFlagImmediateFences;
  native::RewindRing(d);

  CallGuest(ctx, base, __imp__sub_829CEFB0, device_va);
  if (!ctx.r3.u32)
    return fail();
  // (0x82A08028: the GPU interrupt DPC and its thread.)
  CallGuest(ctx, base, __imp__sub_829DC510, device_va);
  if (!ctx.r3.u32)
    return fail();
  // 21560 / 21564 as the hardware path; 14936 as SA2's type-2 path.
  for (const u32 offset : kDeviceOffsetsMinusOne)
    WriteU32(bytes, offset, 0xFFFFFFFFu);
  CallGuest(ctx, base, __imp__sub_82A0A508, device_va);
  if (present_parameters) {
    CallGuest(ctx, base, __imp__sub_82A03DB0, device_va, present_parameters);
    if (!ctx.r3.u32) {
      RGPU_ERROR("Pure device: InitializePresentationParameters failed");
      return fail();
    }
  }
  CallGuest(ctx, base, __imp__sub_82A03AA8, device_va);
  // The kernel's graphics statistics block (import slot 0x83850AF4), when there is one, points at the swap counters.
  if (const u32 slot = LoadGuestU32(0x83850AF4)) {
    if (const u32 holder = LoadGuestU32(slot)) {
      if (const u32 stats = LoadGuestU32(holder + 32)) {
        if (LoadGuestU32(stats) > 8) {
          *GuestPtr<be_u32>(stats + 4) = device_va + 16720;
          *GuestPtr<be_u32>(stats + 8) = device_va + 16724;
        }
        *GuestPtr<be_u32>(stats + 24) = device_va + d3d::kDeviceOffsetSwapCount;
        *GuestPtr<be_u32>(stats + 28) = device_va + 21576;
      }
    }
  }
  native::KickOff(device_va);
  const float frequency = float(KeQueryPerformanceFrequency());
  d.SetF(kDeviceOffsetPerfFrequency, frequency);
  d.SetF(kDeviceOffsetPerfFrequency + 4, frequency > 0.0f ? 1.0f / frequency : 0.0f);
  CallGuest(ctx, base, __imp__sub_82A0E098, device_va);
  // (0x82A13528: VdRetrainEDRAM.)
  CallGuest(ctx, base, __imp__sub_829D4160, device_va, 0);
  // (VdIsHSIOTrainingSucceeded and the EDRAM check 0x82A13830: messages only.)
  native::RewindRing(d);
  // From here the device's fences complete on the render thread (KickFence).
  bytes[kDeviceOffsetFlags2] &= u8(~kDeviceFlagImmediateFences);
  Video::SetGuestDevice(device_va);
  RGPU_INFO("Guest D3D device created at {:#010x} (Renut, no GPU)", device_va);
  ctx.r3.u64 = 1;
}

// The ring segment allocator (0x829E9AF0 (CDevice*, DWORD Dwords); dishonored-nx sub_82A4CCC0; SA2 sub_8254DA90): the
// sink again.
u32 PureNewSegment_hook(u32 device_va, u32 /*dwords*/) {
  native::Device d = native::DeviceAt(device_va);
  if (!d)
    return 0;
  d.SetB(kDeviceOffsetFlags2, d.B(kDeviceOffsetFlags2) & u8(~0x20));
  native::RewindRing(d);
  return d.W(native::off::Ring) + 4;
}

// D3D::SynchronizeToPresentationInterval(CDevice*, BOOL) [0x829F4540; dishonored-nx sub_82A4E8C8; SA2 sub_825589D8]:
// the swap is pending.
void PureSynchronizeToPresentationInterval_hook(u32 device_va, u32 /*immediate*/) {
  native::Device d = native::DeviceAt(device_va);
  if (d)
    d.SetB(d3d::kDeviceOffsetSwapFlags, d.B(d3d::kDeviceOffsetSwapFlags) | d3d::kSwapFlagPending);
}

// The occlusion query's ZPASS_DONE event (0x82A086C0 (CDevice*, query, index); dishonored-nx sub_82A549E0 took the
// query alone). The report: the query's tile count at +148; with at most one tile the report is at *(query + 28) +
// index * 32, else tile t's at *(query + 28 + 4 t) + index * 32, one 32-byte xe_gpu_depth_sample_counts each.
// Nothing writes them without the GPU: a free-running count, as dishonored-nx's (the SDK's EVENT_WRITE_ZPD fallback):
// every report grew since the previous one, so every query passes. The GPU writes the report little-endian.
u32 g_query_counter = 0;

void WriteQueryReport(u32 query_va, u32 index) {
  native::Device q = native::DeviceAt(query_va);
  if (!q)
    return;
  const u32 tiles = std::clamp<u32>(q.W(148), 1, 16);
  const u32 step = u32(std::max<int32_t>(REXCVAR_GET(pure_renut_query_samples), 1));
  g_query_counter += step;
  const u32 lane_a = g_query_counter - (g_query_counter >> 1), lane_b = g_query_counter >> 1;
  for (u32 t = 0; t < tiles; ++t) {
    const u32 tile_base = q.W(28 + t * 4);
    auto *counts = tile_base ? GuestPtr<u32>(tile_base + index * 32) : nullptr;
    if (!counts)
      return;
    const u32 words[8] = {lane_a, lane_b, 0, 0, lane_a, lane_b, 0, 0};  // Total, ZFail, ZPass, StencilFail (A, B)
    std::memcpy(counts, words, sizeof(words));
  }
}

}  // namespace

BANJO_RENUT_HOOK_RAW(sub_82A04440) {  // D3D::InitializeHardwareDevice [dishonored-nx 82A5F9A0; SA2 8255CDA0]
  PureInitializeDevice(ctx, base);
}
BANJO_RENUT_HOOK(sub_829F4540, PureSynchronizeToPresentationInterval_hook);  // [dishonored-nx 82A4E8C8; SA2 825589D8]
BANJO_RENUT_HOOK(sub_829EA048, KickOff_hook);  // D3D_CDevice_KickOff [dishonored-nx 82A4DB18; SA2 8254EBB8]
BANJO_RENUT_HOOK(sub_829E9E60, KickOffSegment_hook);  // D3D_CDevice_KickOffSegment [dishonored-nx 82A4D6D8]
BANJO_RENUT_HOOK(sub_829EA378, BeginRingBig_hook);  // D3D_CDevice_BeginRingBig [dishonored-nx 82A4C528]
BANJO_RENUT_HOOK(sub_829E8970, RingBufferDeviceAllocate_hook);  // D3D_RingBufferDeviceAllocate [dishonored-nx 82A4C8F0]
BANJO_RENUT_HOOK(sub_829E8BF0, AllocateInlineData_hook);  // D3D_CDevice_AllocateInlineData [dishonored-nx 82A4CB20]
BANJO_RENUT_HOOK(sub_829E9AF0, PureNewSegment_hook);  // the ring segment allocator [dishonored-nx 82A4CCC0]
// pure-nx (2026-11-07, the slow motion: the game steps its world once a frame): the guest call chain into Swap, every
// 600th Swap (10 times), as "[pure] swap chain" lines: the return addresses of the frames on the guest stack (back chain
// at 0(r1), each function's saved LR at -8 of its caller's frame), to find Pure's main loop and its update and draw.
BANJO_RENUT_HOOK_RAW(sub_829F4B68) {  // D3DDevice_Swap [dishonored-nx 82A4F610; SA2 82558E98]
  static u32 swaps = 0, logged = 0;
  if (logged < 10 && ++swaps % 600 == 0) {
    ++logged;
    std::string chain = fmt::format("{:08X}", u32(ctx.lr));
    u32 sp = ctx.r1.u32;
    for (int i = 0; i < 24 && sp; ++i) {
      const auto *frame = GuestPtr<const be_u32>(sp);
      if (!frame)
        break;
      const u32 caller_sp = *frame;
      if (caller_sp <= sp || caller_sp - sp > 0x100000)
        break;
      const auto *saved = GuestPtr<const be_u32>(caller_sp - 8);
      if (!saved)
        break;
      const u32 lr = *saved;
      // The outermost frame saves no return (0) and its back chain is the stack's end, unmapped: stop before it
      // (pure_009 crashed reading 70110000, the main thread's stack end).
      if (lr < 0x82000000u || lr >= 0x84000000u)
        break;
      chain += fmt::format(" {:08X}", lr);
      sp = caller_sp;
    }
    RGPU_INFO("[pure] swap chain (Swap {}): {}", swaps, chain);
  }
  ::rex::ppc::HostToGuestFunction<D3DDevice_Swap_hook>(ctx, base);
}
BANJO_RENUT_HOOK(sub_829E9530, BlockOnFence_hook);  // D3D_CDevice_BlockOnFence [dishonored-nx 82A4DC18]
BANJO_RENUT_HOOK(sub_829EA500, PureWaitGpuPending_hook);
// pure-nx (2026-11-07, the slow motion). Pure's frame (sub_823C3350, the game state's vtable +8, called once a frame
// from the main loop sub_823B6C10 through sub_823C2DD8) reads a vertical-blank count, sub_828BAC38 -> [83737800], takes
// the blanks since the last frame, clamped to 1..10, and steps the game by that many 1/60 s ticks (sub_823C2EC8 ->
// the state's vtable +20 with that count). The count is written only by the game's vertical-blank callback
// (sub_828BAC28: *(u32 *)0x83737800 = data->VBlank), which Direct3D calls on every vblank; Renut has no vblank
// interrupt, so the count never moved, every frame was one 1/60 s tick, and the game ran at fps/60 of its speed (a 30
// fps console frame is 2 ticks). Here the read answers the blanks of a 60 Hz display since the first read, from the
// host's steady clock: a frame of 40 ms steps 2 or 3 ticks, as on the console. pure_vblank_clock = false: as before.
REXCVAR_DEFINE_BOOL(pure_vblank_clock, true, "Pure",
                    "Pure's vertical-blank count (its game step: blanks since the last frame) from the host clock at "
                    "60 Hz; off: the count never moves and the game steps one 1/60 s tick a frame")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// pure-nx (2026-11-07, pure_020: a race at 10 fps). Every tick of the game step costs the game thread 12-14 ms here
// (a frame of 2 ticks took 35 ms in pure_018), so a frame longer than the ticks it steps makes the next frame step
// more of them: a hitch (a 1988 ms frame while shaders were missing) or a heavier scene pushed the race into 5-6 ticks
// a frame, 90-100 ms each, and it never came back (the game's own clamp is 10). The count read here advances at most
// pure_vblank_max_steps blanks from the last answer; time beyond that is dropped, so the game slows down for that
// frame instead of stepping more. 3 keeps real speed down to 20 fps; 0: no limit (the game's 1..10).
// pure-nx (2026-11-07, pure_024 with pure_diag_steps_cycle): the race's update steps once with a time step of the
// blanks given (the update 82317290 ignores the count; the game slowed down under the limit), and its cost grows only
// about 5 ms a blank over a fixed 25-30 ms (1 blank 25-31 ms, 2: 34-35, 3: 34-38, 4: 42-46): no spiral (a frame of 16.7
// ms more costs 5 ms more). Default 0 now: the game at its real speed at any frame rate.
REXCVAR_DEFINE_INT32(pure_vblank_max_steps, 0, "Pure",
                     "The most 1/60 s ticks Pure's vertical-blank count advances between two reads (the game steps "
                     "that many a frame): longer frames slow the game instead of stepping more; 0: no limit")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(pure_diag_steps_cycle, false, "Pure",
                    "Diagnostic: the limit of pure_vblank_max_steps cycles 1, 2, 3, 4 every 10 s (the game runs "
                    "slower when the limit is under its frame time); the frame probe logs the update's cost by step")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

u32 PureVblankCount_hook() {
  static const bool on = REXCVAR_GET(pure_vblank_clock);
  if (!on) {
    const auto *count = GuestPtr<const be_u32>(0x83737800u);
    return count ? u32(*count) : 0u;
  }
  using clock = std::chrono::steady_clock;
  static const clock::time_point start = clock::now();
  static const u32 fixed_max_steps = u32(std::max(0, int(REXCVAR_GET(pure_vblank_max_steps))));
  // pure-nx (2026-11-07): pure_diag_steps_cycle: the limit cycles 1, 2, 3, 4 blanks every 10 s (the frame probe logs
  // the update's cost by the blanks it was given: does the game's update cost follow its time step?).
  static const bool cycle = REXCVAR_GET(pure_diag_steps_cycle);
  static std::mutex mutex;
  static uint64_t dropped = 0;  // blanks never answered (beyond max_steps)
  static uint64_t last = 0;     // the last answer
  const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - start).count();
  const uint64_t blanks = uint64_t(ns) * 60u / 1000000000u;
  const u32 max_steps = cycle ? u32(1 + (uint64_t(ns) / 10000000000ull) % 4) : fixed_max_steps;
  std::lock_guard lock(mutex);
  uint64_t count = blanks - dropped;
  if (max_steps && count > last + max_steps) {
    dropped += count - (last + max_steps);
    count = last + max_steps;
  }
  last = std::max(last, count);
  return u32(last);
}
BANJO_RENUT_HOOK(sub_828BAC38, PureVblankCount_hook);  // pure-nx: Pure's vblank count read (lwz r3,[83737800])
  // pure-nx: the wait on device+0x2B00 (no GPU clears it)
BANJO_RENUT_HOOK(sub_829CEEA8, CBlocker_Check_hook);  // D3D_CBlocker_Check [dishonored-nx 82A523B0]
BANJO_RENUT_HOOK(sub_829EA250, IsFencePending_hook);  // D3DDevice_IsFencePending [dishonored-nx 82A4E110]

// The window offset and scissor (0x829D0388 (device, x0, y0, x1, y1); dishonored-nx sub_82A44A98; SA2 sub_8254FCE8):
// Direct3D writes registers 0x2080-0x2082 only into the command buffer, and Renut reads them from the device's window
// packet (+0x28C0). The packet gets what the original writes untiled (offset 0, then the two corners), then the
// original runs into the sink.
BANJO_RENUT_HOOK_RAW(sub_829D0388) {
  const u32 d = ctx.r3.u32;
  const u32 x0 = ctx.r4.u32, y0 = ctx.r5.u32, x1 = ctx.r6.u32, y1 = ctx.r7.u32;
  if (auto *packet = GuestPtr<be_u32>(d + 0x28C0)) {
    packet[0] = 0u;
    packet[1] = ((y0 << 16) & 0x3FFF0000u) | (x0 & 0x3FFFu);
    packet[2] = ((y1 << 16) & 0x3FFF0000u) | (x1 & 0x3FFFu);
  }
  __imp__sub_829D0388(ctx, base);
}

// The occlusion query event (0x82A086C0 (device, query, index)): the original's packets into the sink, then the
// report.
// pure-nx (2026-11-06, the black race: the tone mapping's c44-c48 and c253-c255 were zeros in the device's register
// image): Pure's D3D loads ALU constants for the GPU to read from memory (PM4 LOAD_ALU_CONSTANT, opcode 0x2F) through
// four functions. The packet goes into the ring, which nothing here reads, so the constants never reached the
// register image Renut draws with. After each, the same constants are copied into the image (VertexShaderF for
// registers 0-255, PixelShaderF for 256-511) and their groups marked in m_Pending ([0] vertex, [1] pixel; bit
// 63 - group of four registers), as SetVertex/PixelShaderConstantF leaves them. 'dwords' as the packet's size.
namespace {
u32 PureBe32(u32 va) {
  const auto *p = GuestPtr<const be_u32>(va);
  return p ? u32(*p) : 0;
}
u32 PureBe16(u32 va) {
  const auto *p = GuestPtr<const u8>(va);
  return p ? (u32(p[0]) << 8) | p[1] : 0;
}
void PureLoadAluConstants(u32 device_va, u32 first_reg, u32 src_va, u32 dwords, const char *from) {
  auto *bytes = GuestPtr<u8>(device_va);
  const auto *src = src_va ? GuestPtr<const u8>(src_va) : nullptr;
  // pure-nx (2026-11-06): the first loads, and every load of the tone mapping's c44 (register 300), logged.
  static uint32_t logged = 0;
  const bool tone = first_reg <= 300 && 300 < first_reg + (dwords + 3) / 4;
  static uint32_t tone_logged = 0;
  if (logged < 24 || (tone && tone_logged < 12)) {
    ++logged;
    if (tone)
      ++tone_logged;
    const auto *f = src ? reinterpret_cast<const be_f32 *>(src) : nullptr;
    RGPU_INFO("[pure] ALU load ({}): device {:08X} register {} dwords {} from {:08X}{}{}", from, device_va, first_reg,
              dwords, src_va,
              f ? fmt::format(" first ({:g} {:g} {:g} {:g})", float(f[0]), float(f[1]), float(f[2]), float(f[3]))
                : std::string(" (unmapped)"),
              tone ? " [c44 of the pixel shader]" : "");
  }
  if (!bytes || !src || !dwords || first_reg >= 512)
    return;
  dwords = std::min(dwords, (512 - first_reg) * 4);
  auto &dev = *reinterpret_cast<d3d::Device *>(bytes);
  // pure-nx (2026-11-07, rex_profile.log: 2.8% of the game thread in races, a 4-byte copy and its offsets per dword):
  // each bank's registers are 16 bytes apart in the image, so a bank's part of the load is one copy, and its pending
  // groups one run of bits.
  for (u32 bank = 0; bank < 2 && dwords; ++bank) {
    const u32 bank_end = (bank + 1) * 256;
    if (first_reg >= bank_end)
      continue;
    const u32 n = std::min(dwords, (bank_end - first_reg) * 4);
    const u32 r = first_reg & 255;
    std::memcpy(bytes + offsetof(d3d::Device, m_Constants) +
                    (bank ? offsetof(d3d::Constants, PixelShaderF) : offsetof(d3d::Constants, VertexShaderF)) + r * 16,
                src, size_t(n) * 4);
    const u32 g0 = r / 4, g1 = (r + (n + 3) / 4 - 1) / 4;  // the groups of four registers touched, g0..g1
    const uint64_t run = (g1 - g0 == 63 ? ~0ull : ((2ull << (g1 - g0)) - 1)) << (63 - g1);
    dev.m_Pending[bank] = uint64_t(dev.m_Pending[bank]) | run;
    src += size_t(n) * 4;
    first_reg += n / 4;
    dwords -= n;
  }
}
// The precompiled tables (829F89A0, 829F57D8): at block + off + 20 entries {u16 register, u16 dwords, u32 offset from
// 'base'} up to the first with no dwords (or the table's end).
void PureLoadAluTable(u32 device_va, u32 block, u32 off, u32 base) {
  if (!block || !off)
    return;
  const u32 head = block + off;
  u32 p = head + 20;
  const u32 end = p + PureBe32(head + 16);
  for (u32 n = 0; p < end && n < 4096; ++n) {
    const u32 reg = PureBe16(p), dwords = PureBe16(p + 2);
    if (!dwords)
      break;
    PureLoadAluConstants(device_va, reg, PureBe32(p + 4) + base, dwords, "table");
    p += 8;
  }
}
}  // namespace

// (device, bank 0 vertex 1 pixel, first register, data, vector4 count) [829F5BC8]
BANJO_RENUT_HOOK_RAW(sub_829F5BC8) {
  const u32 device = ctx.r3.u32, reg = ctx.r4.u32 * 256 + ctx.r5.u32, data = ctx.r6.u32, count = ctx.r7.u32;
  __imp__sub_829F5BC8(ctx, base);
  PureLoadAluConstants(device, reg, data, count * 4, "829F5BC8");
}
// (device, bank, first register, vector4 count, constant buffer (its data at +24), first vector4 in it) [829F5A80]
BANJO_RENUT_HOOK_RAW(sub_829F5A80) {
  const u32 device = ctx.r3.u32, reg = ctx.r4.u32 * 256 + ctx.r5.u32, count = ctx.r6.u32, buffer = ctx.r7.u32,
            at = ctx.r8.u32;
  __imp__sub_829F5A80(ctx, base);
  if (buffer)
    PureLoadAluConstants(device, reg, PureBe32(buffer + 24) + at * 16, count * 4, "829F5A80");
}
// (device, block, base): the table at block + *(block + 20) [829F89A0]
BANJO_RENUT_HOOK_RAW(sub_829F89A0) {
  const u32 device = ctx.r3.u32, block = ctx.r4.u32, data = ctx.r5.u32;
  __imp__sub_829F89A0(ctx, base);
  PureLoadAluTable(device, block, PureBe32(block + 20), data);
}
// (device, block, base, image, index): the table at block + *(block + index * 8 + 20) [829F57D8]
BANJO_RENUT_HOOK_RAW(sub_829F57D8) {
  const u32 device = ctx.r3.u32, block = ctx.r4.u32, data = ctx.r5.u32, index = ctx.r7.u32;
  __imp__sub_829F57D8(ctx, base);
  PureLoadAluTable(device, block, PureBe32(block + index * 8 + 20), data);
}

// pure-nx (2026-11-06, the black race, found by the [drawlog] ps constants: the tone mapping's c44-c48 and c253-c255
// zeros): Pure's D3D function at 829F59B0 (device, 0 vertex / 1 pixel, first register, vector4 count) is a
// BeginShaderConstantF4: it reserves ring space (BeginRingBig), writes a PM4 SET_CONSTANT header and returns the data's
// address; the game then writes the values there itself (stvx). Only the GPU would read them; the register image Renut
// draws with never got them. Each reservation is remembered and, before the next draw is captured (QueueDraw,
// QueueDeferredDraw) or the next reservation, its values are copied into the image (PureLoadAluConstants).
namespace {
struct PureRingConstants {
  u32 device, reg, count, data;
};
u8 *PureRegisterBytes(u8 *device, u32 reg) {
  const bool pixel = reg >= 256;
  return device + offsetof(d3d::Device, m_Constants) +
         (pixel ? offsetof(d3d::Constants, PixelShaderF) : offsetof(d3d::Constants, VertexShaderF)) + (reg & 255) * 16;
}
std::mutex g_pure_ring_mutex;
std::vector<PureRingConstants> g_pure_ring;
std::atomic<bool> g_pure_ring_any{false};
}  // namespace

extern "C" void PureFlushRingConstants() {
  if (!g_pure_ring_any.load(std::memory_order_acquire))
    return;
  std::lock_guard<std::mutex> lock(g_pure_ring_mutex);
  // As the GPU sees them at the draw: Direct3D sends the groups of four registers marked in m_Pending (set through
  // SetVertex/PixelShaderConstantF since the last draw) from the register image at the draw, after the ring's loads;
  // every other group keeps what the ring loaded last. So the ring's values go into the image only for groups not
  // pending at this draw, in the order the game reserved them.
  uint64_t pending_at_draw[2] = {0, 0};
  bool read = false;
  for (const PureRingConstants &c : g_pure_ring) {
    u8 *device = GuestPtr<u8>(c.device);
    if (!device)
      continue;
    if (!read) {
      const auto &dev = *reinterpret_cast<const d3d::Device *>(device);
      pending_at_draw[0] = uint64_t(dev.m_Pending[0]);
      pending_at_draw[1] = uint64_t(dev.m_Pending[1]);
      read = true;
    }
    u32 run = 0;
    for (u32 i = 0; i <= c.count; ++i) {
      const u32 reg = c.reg + i;
      const bool take = i < c.count && reg < 512 &&
                        !((pending_at_draw[reg >= 256] >> (63 - (reg & 255) / 4)) & 1);
      if (take) {
        ++run;
        continue;
      }
      if (run)
        PureLoadAluConstants(c.device, reg - run, c.data + (i - run) * 16, run * 4, "829F59B0");
      run = 0;
    }
  }
  g_pure_ring.clear();
  g_pure_ring_any.store(false, std::memory_order_release);
}

BANJO_RENUT_HOOK_RAW(sub_829F59B0) {
  const u32 device = ctx.r3.u32, reg = ctx.r4.u32 * 256 + ctx.r5.u32, count = ctx.r6.u32;
  __imp__sub_829F59B0(ctx, base);
  // The game writes the values after this returns: they are read at the next draw (PureFlushRingConstants).
  const u32 data = ctx.r3.u32;
  if (data && count && reg < 512) {
    std::lock_guard<std::mutex> lock(g_pure_ring_mutex);
    g_pure_ring.push_back({device, reg, std::min(count, 512 - reg), data});
    g_pure_ring_any.store(true, std::memory_order_release);
  }
}

BANJO_RENUT_HOOK_RAW(sub_82A086C0) {
  const u32 query = ctx.r4.u32, index = ctx.r5.u32;
  __imp__sub_82A086C0(ctx, base);
  WriteQueryReport(query, index);
}
