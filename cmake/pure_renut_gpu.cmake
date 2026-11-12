# pure-nx (2026-11-06): Renut's Direct3D-level renderer (github.com/masterspike52/reNut, through banjo-nx, sa2-nx and
# dishonored-nx's src/renut_engine), retargeted to Pure's Direct3D (src/renut_engine: the hooks at Pure's addresses,
# docs/d3d-census.md). From dishonored-nx's cmake/dishonored_renut_gpu.cmake: on the Switch, plume's deko3d back end.
#
# The third-party code (plume, uam, reblue-XenosRecomp, imgui) and the shader tools are sa2-nx's, used where they are:
# RENUT_SA2NX_APP_DIR, sa2-nx's app folder. By default ..\sa2-nx\app beside this project (the one dishonored-nx
# builds with: its plume has the deko3d changes Renut's deko3d glue calls), else the app folder of the SDK this build
# uses.
#
# An OBJECT library, not a static one: its hooks replace the generated code's weak sub_X definitions, and a static
# archive's members are only pulled in for undefined symbols, so the hooks would silently not be linked.

# In this repository: tools/rexglue-sdk-pure/app (the SDK fork's app folder, its third-party code).
get_filename_component(_pure_sa2nx_app "${REXSDK_DIR}/../app" ABSOLUTE)
set(RENUT_SA2NX_APP_DIR "${_pure_sa2nx_app}" CACHE PATH "sa2-nx's app folder (Renut's third-party code and tools)")
foreach(_pure_need thirdparty/plume/CMakeLists.txt thirdparty/reblue-XenosRecomp/CMakeLists.txt
                   thirdparty/imgui/imgui.cpp cmake/collect_captured_shaders.cmake)
    if(NOT EXISTS "${RENUT_SA2NX_APP_DIR}/${_pure_need}")
        message(FATAL_ERROR "Renut: ${RENUT_SA2NX_APP_DIR}/${_pure_need} missing (sa2-nx's app folder with its "
                            "submodules: git submodule update --init --recursive in sa2-nx; or set RENUT_SA2NX_APP_DIR)")
    endif()
endforeach()
message(STATUS "pure: Renut's third-party code from ${RENUT_SA2NX_APP_DIR}")

# The game's shader containers Renut's translator builds its cache from (global_shader_combos.fxc and the
# creation-time dumps, pure_dump_shaders): one container a file, vs_/ps_<hash>.bin. Must exist (the dump tool of
# gpu.cmake is N&B's / SA2's). The Switch build reads only switch_shaders/, made from it on Linux.
set(RENUT_SHADER_DUMP_DIR "${CMAKE_SOURCE_DIR}/renut_shaders" CACHE PATH "Pure's shader containers for Renut")
if(NOT EXISTS "${RENUT_SHADER_DUMP_DIR}")
    message(FATAL_ERROR "Renut: no ${RENUT_SHADER_DUMP_DIR}: unpack the shader containers there (renut_shaders.zip)")
endif()
set(RENUT_ASSETS_DIR "${CMAKE_SOURCE_DIR}/assets/game_root" CACHE PATH "Game assets")

# Renut's XenosRecomp is built as an ExternalProject (gpu.cmake): it gets this build's whole host toolchain.
set(BANJO_RENUT_XENOS_RECOMP_ARGS "")
foreach(_banjo_var CMAKE_C_COMPILER_TARGET CMAKE_CXX_COMPILER_TARGET CMAKE_LINKER_TYPE CMAKE_C_FLAGS CMAKE_CXX_FLAGS
                   CMAKE_AR CMAKE_RC_COMPILER CMAKE_MT)
    if(DEFINED ${_banjo_var} AND NOT "${${_banjo_var}}" STREQUAL "")
        list(APPEND BANJO_RENUT_XENOS_RECOMP_ARGS "-D${_banjo_var}=${${_banjo_var}}")
    endif()
endforeach()

# Renut's SDK exports o1heap as rex::o1heap; banjo-nx's SDK fork names it o1heap.
if(TARGET o1heap AND NOT TARGET rex::o1heap)
    add_library(rex::o1heap ALIAS o1heap)
endif()

# dishonored-nx (2026-10-29), pure-nx: the Switch build (sa2-nx's cmake/sa2_renut_gpu.cmake, Switch part). A cross build: the
# shader translator (XenosRecomp) and the deko3d shader tools are host programs, so their output comes ready made in
# switch_shaders/ (made on Linux from renut_shaders/ with the same XenosRecomp, DXC, SPIRV-Cross and uam):
#   shader_cache_switch.cpp.in + spirv_cache.zst   Renut's shader cache (SPIR-V only; the console reads no DXIL)
#   deko3d_glsl_switch.cpp.in + deko3d_glsl.bin    the game shaders' GLSL for deko3d (uam compiles them on the console)
#   host_shaders/*.deko3d.h                        Renut's own shaders for deko3d
# Renut always draws with deko3d here (REXGLUE_SWITCH_NO_NVK: no Vulkan driver in the NRO).
if(REXGLUE_PLATFORM_SWITCH)
    set(PURE_SWITCH_SHADER_DIR "${CMAKE_SOURCE_DIR}/switch_shaders" CACHE PATH
        "The ready-made Switch shader files (switch_shaders, made on Linux from renut_shaders)")
    foreach(_pure_switch_need shader_cache_switch.cpp.in spirv_cache.zst deko3d_glsl_switch.cpp.in deko3d_glsl.bin
                             host_shaders/present_ps.deko3d.h)
        if(NOT EXISTS "${PURE_SWITCH_SHADER_DIR}/${_pure_switch_need}")
            message(FATAL_ERROR "Renut (Switch): no ${PURE_SWITCH_SHADER_DIR}/${_pure_switch_need}")
        endif()
    endforeach()
    set(PLUME_PLATFORM_SWITCH ON CACHE BOOL "" FORCE)
    set(PLUME_SWITCH_DEVKITPRO "${DEVKITPRO}" CACHE PATH "" FORCE)
    set(PLUME_SWITCH_NVK_ROOT "${REXGLUE_SWITCH_NVK_SDK}" CACHE PATH "" FORCE)
    set(PLUME_SWITCH_NVK_LIBRARY "${REXGLUE_SWITCH_NVK_SDK}/lib/libvulkan.a" CACHE FILEPATH "" FORCE)
    set(PLUME_SWITCH_NO_NVK "${REXGLUE_SWITCH_NO_NVK}" CACHE BOOL "" FORCE)
    set(PLUME_SWITCH_UAM_DIR "${RENUT_SA2NX_APP_DIR}/thirdparty/uam" CACHE PATH "" FORCE)
    set(PLUME_DEKO3D ON CACHE BOOL "deko3d back end (Switch)" FORCE)
    set(BANJO_DEKO3D_HOST_SHADERS_PREBUILT "${PURE_SWITCH_SHADER_DIR}/host_shaders")
    # The .incbin paths of the two embedded files: forward slashes for the assembler.
    file(TO_CMAKE_PATH "${PURE_SWITCH_SHADER_DIR}" PURE_SWITCH_SHADER_DIR_FWD)
    set(_pure_saved_dir "${PURE_SWITCH_SHADER_DIR}")
    set(PURE_SWITCH_SHADER_DIR "${PURE_SWITCH_SHADER_DIR_FWD}")
    set(BANJO_DEKO3D_GLSL_CPP "${CMAKE_BINARY_DIR}/pure_switch/deko3d_glsl_switch.cpp")
    set(BANJO_RENUT_SHADER_CACHE_CPP "${CMAKE_BINARY_DIR}/pure_switch/shader_cache_switch.cpp")
    configure_file("${_pure_saved_dir}/deko3d_glsl_switch.cpp.in" "${BANJO_DEKO3D_GLSL_CPP}" @ONLY)
    configure_file("${_pure_saved_dir}/shader_cache_switch.cpp.in" "${BANJO_RENUT_SHADER_CACHE_CPP}" @ONLY)
    set(PURE_SWITCH_SHADER_DIR "${_pure_saved_dir}")
    # A change of the embedded files rebuilds the two sources (the assembler reads them, CMake does not see that).
    set_source_files_properties("${BANJO_DEKO3D_GLSL_CPP}" PROPERTIES
        OBJECT_DEPENDS "${PURE_SWITCH_SHADER_DIR}/deko3d_glsl.bin" SKIP_PRECOMPILE_HEADERS ON)
    set_source_files_properties("${BANJO_RENUT_SHADER_CACHE_CPP}" PROPERTIES
        OBJECT_DEPENDS "${PURE_SWITCH_SHADER_DIR}/spirv_cache.zst" SKIP_PRECOMPILE_HEADERS ON)
    function(banjo_renut_shader_cache_override TARGET)
        target_sources(${TARGET} PRIVATE "${BANJO_RENUT_SHADER_CACHE_CPP}")
    endfunction()
endif()

# banjo-nx's early alpha pass over the pixel shaders (tools/renut_early_alpha.py, two translator runs) is off for now:
# the first PC runs compare against the emulated GPU without it, and the build needs no Python.
set(BANJO_RENUT_EARLY_ALPHA OFF)

include("${CMAKE_SOURCE_DIR}/src/renut_engine/gpu/gpu.cmake")

add_library(pure_renut_gpu OBJECT
    "${CMAKE_SOURCE_DIR}/src/renut_engine/gpu/banjo_renut_select.cpp"
    "${CMAKE_SOURCE_DIR}/src/renut_engine/gpu/banjo_renut_tuning.cpp"
    "${CMAKE_SOURCE_DIR}/src/renut_engine/gpu/banjo_renut_checks.cpp"
    "${CMAKE_SOURCE_DIR}/src/renut_engine/gpu/banjo_renut_record.cpp"
    "${CMAKE_SOURCE_DIR}/src/renut_engine/display_sync.cpp"
    "${CMAKE_SOURCE_DIR}/src/renut_engine/perf/frame_stats.cpp"
    "${CMAKE_SOURCE_DIR}/src/renut_engine/sa2_compat/frame_latency.cpp"
    "${CMAKE_SOURCE_DIR}/src/renut_engine/sa2_compat/sa2_compat.cpp"
    "${CMAKE_SOURCE_DIR}/src/renut_engine/gpu/sa2_renut_pacing.cpp")
target_include_directories(pure_renut_gpu PRIVATE
    "${CMAKE_SOURCE_DIR}"
    "${CMAKE_SOURCE_DIR}/src"
    "${CMAKE_SOURCE_DIR}/src/renut_engine/sa2_compat"
    "${REXSDK_DIR}/src/core")
target_link_libraries(pure_renut_gpu PRIVATE rex::runtime)
if(REXGLUE_PLATFORM_SWITCH)
    # The deko3d back end's glue and the game shaders' GLSL package (sa2-nx's cmake/sa2_renut_gpu.cmake).
    target_sources(pure_renut_gpu PRIVATE
        "${CMAKE_SOURCE_DIR}/src/renut_engine/gpu/banjo_renut_deko3d.cpp"
        "${BANJO_DEKO3D_GLSL_CPP}")
    target_include_directories(pure_renut_gpu PRIVATE "${REXGLUE_ENTRYPOINT_INCLUDE_DIR}")
    if(REXGLUE_SWITCH_NO_NVK)
        # No Vulkan driver in the NRO: Renut on deko3d whatever renut_gpu_backend says.
        set_property(SOURCE "${CMAKE_SOURCE_DIR}/src/renut_engine/gpu/banjo_renut_select.cpp"
                            "${RENUT_GPU_DIR}/backend/backend.cpp"
                     APPEND PROPERTY COMPILE_DEFINITIONS SA2_SWITCH_NO_NVK=1)
    endif()
endif()
if(COMMAND rexglue_apply_target_settings)
    rexglue_apply_target_settings(pure_renut_gpu)
endif()
renut_gpu_setup(pure_renut_gpu)
target_compile_definitions(pure_renut_gpu PRIVATE SA2_RENUT_RENDERER=1 PURE_RENUT_RENDERER=1)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(pure_renut_gpu PRIVATE -ffp-contract=off -fno-strict-aliasing)
endif()
target_link_libraries(pure PRIVATE pure_renut_gpu)
target_compile_definitions(pure PRIVATE SA2_RENUT_RENDERER=1 PURE_RENUT_RENDERER=1)
target_include_directories(pure PRIVATE "${CMAKE_SOURCE_DIR}/src/renut_engine/sa2_compat")
