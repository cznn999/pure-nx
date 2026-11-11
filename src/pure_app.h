#pragma once

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string_view>
#include <system_error>

#include <rex/filesystem.h>
#include <rex/input/device_assignment.h>
#include <rex/input/input_system.h>
#include <rex/logging.h>
#include <rex/rex_app.h>
#include <rex/runtime.h>
#include <rex/system/function_dispatcher.h>

#include "pure_pgo.h"  // pure-nx (2026-11-07): PGO profile collection (PURE_PGO=generate only)
#if defined(PURE_BUILTIN_CONFIG)
#include "pure_builtin_config.h"  // generated from pure_switch_defaults.toml (CMake)
#endif

#if PURE_RENUT_RENDERER
// pure-nx (2026-11-06): Renut's Direct3D-level renderer (src/renut_engine, from reNut by masterspike52 through banjo-nx,
// sa2-nx and dishonored-nx); on the Switch always, on deko3d.
#include "renut_engine/gpu/banjo_renut_select.h"
#include "renut_engine/gpu/overlay/app_integration.h"
#endif

#include <rex/platform.h>
#if REX_PLATFORM_SWITCH
#include <rex/cvar.h>
#include <rex/ui/switch_apm.h>

REXCVAR_DEFINE_BOOL(pure_gpu_boost, true, "Pure",
                    "Handheld: ask for the official 460.8 MHz GPU profile (memory clock checked unchanged, else "
                    "reverted); docked: nothing")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
#endif

// Included by src/main.cpp after generated/default/pure_init.h (PPCImageConfig).
class PureApp final : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<PureApp>(new PureApp(ctx, "pure", PPCImageConfig));
  }

 protected:
  // The Switch build's fixed settings (pure_switch_defaults.toml, compiled in): applied before pure.toml.
  std::string_view BuiltInConfig() const override {
#if defined(PURE_BUILTIN_CONFIG)
    return kPureBuiltInConfig;
#else
    return {};
#endif
  }
  // Without a pure.toml beside the NRO, the player's default one is written there (pure_switch.toml).
  std::string_view DefaultConfigFile() const override {
#if defined(PURE_BUILTIN_CONFIG)
    return kPurePlayerConfig;
#else
    return {};
#endif
  }

  // pure-nx (2026-11-06, dishonored-nx's dishonored_app.h): without --game_data_root / game_data_root in pure.toml,
  // a game_root/ folder beside the executable or above it (on the Switch sdmc:/switch/pure-nx/game_root).
  void OnConfigurePaths(rex::PathConfig& paths) override {
    if (!paths.game_data_root.empty()) return;
    std::error_code ec;
    const auto folder = rex::filesystem::GetExecutableFolder();
    if (folder.empty()) return;
    for (auto candidate = folder; !candidate.empty(); candidate = candidate.parent_path()) {
      if (std::filesystem::is_regular_file(candidate / "game_root" / "default.xex", ec)) {
        paths.game_data_root = candidate / "game_root";
        return;
      }
      if (candidate == candidate.parent_path()) break;
    }
  }

  void OnPostLoadXexImage() override { RegisterJumpStubs(); }

  // pure-nx (2026-11-06, dishonored-nx's dishonored_app.h): with Renut's renderer there is no SDK graphics system: Renut
  // has its own swapchain and render thread.
  void OnPreSetup(rex::RuntimeConfig& config) override {
    pure::pgo::Start();  // pure-nx: nothing unless the build is the PGO-instrumented one (build_switch_pgo.cmd generate)
#if REX_PLATFORM_SWITCH
    // pure-nx (2026-11-07, renut-nx's banjo_gpu_boost / sa2-nx's sa2_gpu_boost): the official handheld GPU profile,
    // 0x92220008 (GPU 460.8 MHz, memory 1331.2 MHz, as commercial games ask for), through the SDK's switch_apm.cpp: it
    // checks the real clocks for 1.5 s that the memory clock did not move, reverts if it did, and asks nothing when
    // docked; it logs what it did on stderr. Nothing asked for it here: rex_profile.log showed GPU 307.2 MHz in races.
    if (REXCVAR_GET(pure_gpu_boost)) {
      RexSwitchApmRequestGpuMhz(460);
      RexSwitchApmApply();
    }
#endif
#if PURE_RENUT_RENDERER
    if (banjo::renut_gpu::Active()) {
      renut::gpu::ConfigureRuntime(config);
      REXLOG_INFO("graphics: Renut's Direct3D-level renderer (no PM4 ring, no EDRAM)");
    }
#else
    (void)config;
#endif
  }

  // dishonored-nx (2026-10-31): every controller drives guest user 0, the one signed in (the SDK's SharedAssignment).
  void OnPostSetup() override {
    if (auto* input = static_cast<rex::input::InputSystem*>(runtime() ? runtime()->input_system() : nullptr)) {
      input->SetDeviceAssignment(std::make_unique<rex::input::SharedAssignment>());
      REXLOG_INFO("input: every controller is player 1");
    }
  }

#if PURE_RENUT_RENDERER
  // Renut's renderer draws the SDK's menus through its own plume drawer, resizes its swapchain itself, and starts once
  // the window exists (before the game: the paths are final).
  std::unique_ptr<rex::ui::ImmediateDrawer> OnCreateImmediateDrawer() override {
    if (banjo::renut_gpu::Active()) return renut::gpu::CreateOverlayDrawer();
    return rex::ReXApp::OnCreateImmediateDrawer();
  }
  void OnWindowPixelSizeChanged(uint32_t pixel_width, uint32_t pixel_height) override {
    rex::ReXApp::OnWindowPixelSizeChanged(pixel_width, pixel_height);
    if (banjo::renut_gpu::Active()) renut::gpu::OnWindowResized();
  }
  std::optional<rex::PathConfig> OnFinalizePaths(const rex::PathConfig& defaults,
                                                 std::function<void(rex::PathConfig)> resume) override {
    if (banjo::renut_gpu::Active()) renut::gpu::StartRenderer(app_context(), [this] { return imgui_drawer(); });
    return rex::ReXApp::OnFinalizePaths(defaults, std::move(resume));
  }
  void OnShutdown() override {
    if (banjo::renut_gpu::Active()) renut::gpu::ShutdownRenderer();
  }
#endif

 private:
  // pure-nx (2026-11-06): Pure's code range begins (0x821A0010-0x822FFFFF) and ends (0x82F00000-0x835A34AC) with
  // about 357,000 jump stubs of four instructions: lis r11,hi / addi r11,r11,lo / mtctr r11 / bctr. The game calls
  // through them (direct calls, vtables, thread entry points). The generator only registered the ~23,000 it reached
  // by direct calls; the first boot stopped at the thread entry 0x82254D60, one of the others. Generating C++ for all
  // of them would add ~334,000 functions; instead each unregistered stub gets its target's host function in the
  // function table (stubs to stubs are followed). The stub's only effects besides the jump are r11 and CTR set to
  // the target; a function does not read either at its entry, so the call is the same. Stubs whose target is not a
  // registered function (319 point at zero words: stubs of removed code) stay unregistered and still fail loudly.
  void RegisterJumpStubs() {
    auto* dispatcher = runtime()->function_dispatcher();
    const uint32_t lo = static_cast<uint32_t>(PPCImageConfig.code_base);
    const uint32_t hi = lo + static_cast<uint32_t>(PPCImageConfig.code_size);
    const uint8_t* code = runtime()->memory()->TranslateVirtual<const uint8_t*>(lo);
    auto word = [&](uint32_t address) {
      uint32_t v;
      std::memcpy(&v, code + (address - lo), 4);
      return __builtin_bswap32(v);
    };
    auto stub_target = [&](uint32_t address, uint32_t& target) {
      if (address < lo || address + 16 > hi) {
        return false;
      }
      const uint32_t w0 = word(address), w1 = word(address + 4);
      if ((w0 >> 16) != 0x3D60u || (w1 >> 16) != 0x396Bu || word(address + 8) != 0x7D6903A6u ||
          word(address + 12) != 0x4E800420u) {
        return false;
      }
      target = (w0 << 16) + static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(w1 & 0xFFFFu)));
      return true;
    };
    uint32_t mapped = 0, unresolved = 0;
    for (uint32_t address = lo; address + 16 <= hi; address += 4) {
      uint32_t target;
      if (!stub_target(address, target) || dispatcher->GetFunction(address)) {
        continue;
      }
      ::PPCFunc* func = nullptr;
      for (int depth = 0; depth < 8; ++depth) {
        if (target >= lo && target < hi && (func = dispatcher->GetFunction(target))) {
          break;
        }
        if (!stub_target(target, target)) {
          break;
        }
      }
      if (func && dispatcher->SetFunction(address, func)) {
        ++mapped;
      } else {
        ++unresolved;
      }
    }
    REXLOG_INFO("pure: {} jump stubs mapped to their targets, {} left unregistered (no registered target)", mapped,
                unresolved);
  }
};
