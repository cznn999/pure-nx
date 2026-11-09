cmake_minimum_required(VERSION 3.25)
project(pure_xex_exporter LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(REXGLUE_SDK_ROOT "" CACHE PATH "Existing ReXGlue SDK checkout")
set(REXGLUE_SDK_LIB_DIR "" CACHE PATH "Directory containing prebuilt host SDK libraries")
if(NOT EXISTS "${REXGLUE_SDK_ROOT}/include/rex/runtime.h")
    message(FATAL_ERROR "Set REXGLUE_SDK_ROOT to the existing SDK checkout")
endif()
if(NOT EXISTS "${REXGLUE_SDK_LIB_DIR}/rexruntimerd.lib")
    message(FATAL_ERROR "Prebuilt host library not found under REXGLUE_SDK_LIB_DIR")
endif()

add_library(rexruntime_host SHARED IMPORTED GLOBAL)
set_target_properties(rexruntime_host PROPERTIES
    IMPORTED_IMPLIB "${REXGLUE_SDK_LIB_DIR}/rexruntimerd.lib"
    IMPORTED_LOCATION "${REXGLUE_SDK_LIB_DIR}/rexruntimerd.dll"
)

add_executable(pure_xex_exporter main.cpp)
target_include_directories(pure_xex_exporter PRIVATE
    "${REXGLUE_SDK_ROOT}/include"
    "${REXGLUE_SDK_ROOT}/thirdparty/fmt/include"
    "${REXGLUE_SDK_ROOT}/thirdparty/spdlog/include"
    "${REXGLUE_SDK_ROOT}/thirdparty/simde"
    "${REXGLUE_SDK_ROOT}/thirdparty/simde/simde"
    "${REXGLUE_SDK_ROOT}/thirdparty/tomlplusplus/include"
    "${REXGLUE_SDK_ROOT}/thirdparty/tracy/public"
)
target_link_libraries(pure_xex_exporter PRIVATE rexruntime_host)

add_custom_command(TARGET pure_xex_exporter POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${REXGLUE_SDK_LIB_DIR}/rexruntimerd.dll"
        "$<TARGET_FILE_DIR:pure_xex_exporter>/rexruntimerd.dll"
    VERBATIM
)
if(EXISTS "${REXGLUE_SDK_LIB_DIR}/TracyClientrd.dll")
    add_custom_command(TARGET pure_xex_exporter POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${REXGLUE_SDK_LIB_DIR}/TracyClientrd.dll"
            "$<TARGET_FILE_DIR:pure_xex_exporter>/TracyClientrd.dll"
        VERBATIM
    )
endif()
