cmake_minimum_required(VERSION 3.25)
project(pure LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(_pure_local_rexglue_sdk "${CMAKE_CURRENT_SOURCE_DIR}/tools/rexglue-sdk-pure/sdk")

if((NOT DEFINED REXSDK_DIR OR REXSDK_DIR STREQUAL "")
   AND EXISTS "${_pure_local_rexglue_sdk}/CMakeLists.txt")
    set(REXSDK_DIR "${_pure_local_rexglue_sdk}" CACHE PATH
        "Path to the ReXGlue SDK source tree" FORCE)
endif()

# Switch (devkitA64 GCC): no AArch64 early register allocation pass (sa2-nx / dishonored-nx: it miscompiled table
# lookups of the XDK's streaming copy on the console). Before the SDK is added, so its libraries get it too.
if(REXGLUE_PLATFORM_SWITCH)
    add_compile_options(-mearly-ra=none)
    add_link_options(-mearly-ra=none)
endif()

include(generated/rexglue.cmake)

# The SDK's windowed entry point is wWinMain on Windows (src/ui/windowed_app_main_sdl.cpp): a console-subsystem
# executable looks for main and fails to link ("lld-link: undefined symbol: main"). Same as dishonored-nx.
set(PURE_SOURCES
    src/main.cpp
    # pure-nx (2026-11-06): the shader dump around Renut's shader setup hooks (pure_dump_shaders).
    src/pure_renut_hooks.cpp
    # What Renut's renderer takes from sa2-nx's app that Pure does not have (as dishonored-nx's
    # dishonored_renut_compat.cpp and dishonored_me_stubs.cpp).
    src/pure_renut_compat.cpp
    # Whether the generated code marks the store cards (tools/store_cards.py): Renut's cheap exact stream checks.
    src/pure_store_cards.cpp
    # Where Pure's frame goes on the main thread (pure_frame_probe): update / draw / end of frame, the waits.
    src/pure_frame_probe.cpp
    src/pure_native.cpp  # pure-nx (2026-11-09): native replacements of hot guest leaf functions
    # PGO profile collection (PURE_PGO=generate; empty otherwise). renut-nx's / sa2-nx's.
    src/pure_pgo.cpp
    # sa2-nx's app parts Renut's renderer calls (dishonored-nx's copies): the console's display mode and render
    # heights, and the enhancement switches.
    src/me/sa2_render_mode.cpp
    src/cpu/sa2_enhancements.cpp
)
if(WIN32)
    add_executable(pure WIN32 ${PURE_SOURCES})
else()
    add_executable(pure ${PURE_SOURCES})
endif()
rexglue_setup_target(pure GPU_PLUGINS xenos)
target_include_directories(pure PRIVATE src)

# pure-nx (2026-11-06): Renut's Direct3D-level renderer (src/renut_engine; cmake/pure_renut_gpu.cmake), as
# dishonored-nx's: on the Switch it draws with deko3d (no Vulkan driver in the NRO), chosen with pure_renderer =
# "renut" (pure_switch_defaults.toml). Needs sa2-nx's app folder (its third-party code) and switch_shaders/.
option(PURE_RENUT_RENDERER "Link Renut's Direct3D-level renderer (pure_renderer = renut)" ON)
if(PURE_RENUT_RENDERER)
    include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/pure_renut_gpu.cmake")
    # src/pure_renut_hooks.cpp hashes the dumped shaders with the xxHash Renut keys its shader cache with
    # (XXH_INLINE_ALL: the header alone). The SDK's xxhash target exports no include directory here.
    target_include_directories(pure BEFORE PRIVATE "${RENUT_XXHASH_DIR}")
endif()

# As sa2-nx / dishonored-nx: no FMA contraction and no strict aliasing for the recompiled code (rounding as on the
# Xbox 360).
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    foreach(_t pure pure_recomp)
        if(TARGET ${_t})
            target_compile_options(${_t} PRIVATE -ffp-contract=off -fno-strict-aliasing)
        endif()
    endforeach()
endif()

# pure-nx (2026-11-06): the Nintendo Switch NRO (dishonored-nx's CMakeLists.txt, Switch part), built by
# build_switch.cmd. With Renut on deko3d (above).
if(REXGLUE_PLATFORM_SWITCH)
    # An emulator starts NROs without argv[0]: the SDK then takes the NRO's usual place on the SD for its folder.
    if(TARGET rexcore)
        target_compile_definitions(rexcore PRIVATE REX_SWITCH_FALLBACK_EXE="/switch/pure-nx/pure.nro")
    endif()
    # The register file is one generated function of tens of thousands of SetFunction calls run once at start:
    # -O0 compiles it in seconds (GCC -O3 spent a quarter of an hour on SA2's).
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/generated/default/pure_register.cpp")
        set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/generated/default/pure_register.cpp"
                     APPEND PROPERTY COMPILE_OPTIONS -O0)
    endif()
    # pure-nx (2026-11-07): the same for pure_init.cpp, a table of 98,000 function pointers (PPCFuncMappings): at -O3 cc1plus
    # ran out of memory on it beside the game files of a full rebuild ("out of memory allocating 2097112 bytes").
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/generated/default/pure_init.cpp")
        set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/generated/default/pure_init.cpp"
                     APPEND PROPERTY COMPILE_OPTIONS -O0)
    endif()
    # The game code without the x29 frame chain and without the errno check of sqrt (sa2-nx's defaults).
    if(TARGET pure_recomp)
        target_compile_options(pure_recomp PRIVATE -fomit-frame-pointer -fno-math-errno)
        # pure-nx (2026-11-07): generated\default\pure_pch.h after tools\pch_no_volatile.py reads guest memory without
        # volatile, with tools\loop_barriers.py's barrier at the loop headers; ON gives the generated volatile back.
        option(PURE_GUEST_VOLATILE "Guest loads and stores through volatile pointers, as generated" OFF)
        if(PURE_GUEST_VOLATILE)
            target_compile_definitions(pure_recomp PRIVATE REX_GUEST_VOLATILE=volatile)
        endif()
    endif()
    # The built-in settings (pure_switch_defaults.toml) and the player's pure.toml written beside the NRO when there
    # is none (pure_switch.toml).
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/pure_switch_defaults.toml" PURE_BUILTIN_CONFIG)
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/pure_switch.toml" PURE_PLAYER_CONFIG)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/pure_switch_defaults.toml" "${CMAKE_CURRENT_SOURCE_DIR}/pure_switch.toml")
    configure_file("${CMAKE_CURRENT_SOURCE_DIR}/src/pure_builtin_config.h.in"
                   "${CMAKE_CURRENT_BINARY_DIR}/builtin_config/pure_builtin_config.h" @ONLY)
    target_include_directories(pure PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/builtin_config")
    target_compile_definitions(pure PRIVATE PURE_BUILTIN_CONFIG=1)
    # As dishonored-nx: xxHash (Renut's shader keys) without unaligned loads the LTO stage could merge.
    target_compile_definitions(pure PRIVATE XXH_FORCE_MEMORY_ACCESS=0)

    # pure-nx (2026-11-07, renut-nx's app/CMakeLists.txt and docs/pgo-lto.md, sa2-nx's method): link-time and
    # profile-guided optimisation, in build folders of their own (build_switch_pgo.cmd); out\nx stays without both.
    #   - PURE_SWITCH_LTO: the game code (pure_recomp), the app (pure) and Renut's renderer (pure_renut_gpu) compiled to
    #     GIMPLE and optimised as one program at the link (-flto=N -flto-partition=balanced; N low for the PC's memory).
    #   - PURE_PGO=generate: an instrumented NRO that streams its branch counts to pgo/ next to the NRO (src/pure_pgo.cpp;
    #     -fprofile-info-section: libgcov's exit-time files do not fit the Switch; no value profiling, not thread-safe).
    #   - PURE_PGO=use: the counts merged on the PC (tools/pgo_merge.py) from PURE_PGO_DIR (pgo\ at the root). Code the
    #     sessions did not run is compiled as without a profile (-fprofile-partial-training); functions changed since
    #     keep none (-Wno-coverage-mismatch). With a profile, the game code's inliner limits of sa2-nx (64 / 12 %).
    # Neither changes what the code computes: the same source, compiled with better information.
    option(PURE_SWITCH_LTO "Link-time optimisation of the game code, the app and Renut's renderer (Switch)" OFF)
    set(PURE_SWITCH_LTO_JOBS "2" CACHE STRING "Parallel LTRANS jobs of the Switch LTO link")
    set(PURE_PGO "" CACHE STRING "Profile-guided optimisation of the Switch build: empty, generate or use")
    set_property(CACHE PURE_PGO PROPERTY STRINGS "" generate use)
    set(PURE_PGO_DIR "${CMAKE_CURRENT_SOURCE_DIR}/pgo" CACHE PATH "Folder of the PGO profile (.gcda files)")
    set(_pure_opt_targets pure pure_recomp pure_renut_gpu)
    if(PURE_SWITCH_LTO)
        foreach(_t ${_pure_opt_targets})
            if(TARGET ${_t})
                target_compile_options(${_t} PRIVATE -flto -fno-fat-lto-objects)
            endif()
        endforeach()
        target_link_options(pure PRIVATE -flto=${PURE_SWITCH_LTO_JOBS} -flto-partition=balanced -ffp-contract=off
                            -fno-strict-aliasing -fno-math-errno)
        message(STATUS "pure: Switch LTO (${PURE_SWITCH_LTO_JOBS} jobs)")
    endif()
    get_filename_component(_pure_pgo_dir "${PURE_PGO_DIR}" ABSOLUTE)
    file(TO_NATIVE_PATH "${_pure_pgo_dir}" _pure_pgo_dir)
    file(TO_NATIVE_PATH "${CMAKE_BINARY_DIR}" _pure_pgo_prefix)
    if(PURE_PGO STREQUAL "generate")
        set(_pure_pgo "-fprofile-generate=${_pure_pgo_dir}" "-fprofile-prefix-path=${_pure_pgo_prefix}"
                      -fno-profile-values -fprofile-update=single -fprofile-info-section=gcov_info)
        target_compile_definitions(pure PRIVATE PURE_PGO_GENERATE=1)
    elseif(PURE_PGO STREQUAL "use")
        if(NOT EXISTS "${PURE_PGO_DIR}")
            message(FATAL_ERROR "PURE_PGO=use: no profile in ${PURE_PGO_DIR} (tools/pgo_merge.py)")
        endif()
        # -fprofile-correction: counts taken without atomic updates while several threads ran the same code.
        set(_pure_pgo "-fprofile-use=${_pure_pgo_dir}" "-fprofile-prefix-path=${_pure_pgo_prefix}"
                      -fprofile-correction -fno-profile-values -fprofile-partial-training -Wno-missing-profile
                      -Wno-coverage-mismatch)
        if(TARGET pure_recomp)
            target_compile_options(pure_recomp PRIVATE --param=max-inline-insns-auto=64 --param=inline-unit-growth=12)
        endif()
    elseif(PURE_PGO)
        message(FATAL_ERROR "PURE_PGO must be empty, generate or use (got ${PURE_PGO})")
    endif()
    if(PURE_PGO)
        foreach(_t ${_pure_opt_targets})
            if(TARGET ${_t})
                target_compile_options(${_t} PRIVATE ${_pure_pgo})
            endif()
        endforeach()
        target_link_options(pure PRIVATE ${_pure_pgo})
        message(STATUS "pure: PGO ${PURE_PGO} (${PURE_PGO_DIR})")
    endif()

    # The NRO's icon: icon.jpg beside this file (256x256 JPEG), else libnx's default one.
    set(PURE_NRO_ICON_ARGS)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/icon.jpg")
        set(PURE_NRO_ICON_ARGS ICON "${CMAKE_CURRENT_SOURCE_DIR}/icon.jpg")
    endif()
    rexglue_switch_add_nro(pure
        NAME "Pure"
        AUTHOR "pure-nx"
        VERSION "0.20"
        ${PURE_NRO_ICON_ARGS})
endif()
