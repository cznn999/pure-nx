@echo off
rem pure-nx: builds Pure for the Nintendo Switch: out\nx\pure.nro (devkitA64 GCC + libnx).
rem From dishonored-nx's build_switch.cmd. Uses the generated code in generated\default (codegen.cmd makes it) and
rem pure-nx's own SDK (tools\rexglue-sdk-pure\sdk). The whole output goes to out\build_switch.log.
rem 2026-11-06: with Renut's renderer on deko3d (cmake\pure_renut_gpu.cmake): needs ..\sa2-nx\app (plume, uam,
rem XenosRecomp's headers: the same as dishonored-nx builds with) and switch_shaders\ (made on Linux from renut_shaders\).
setlocal EnableDelayedExpansion
cd /d "%~dp0"
if not exist out mkdir out
echo [switch] building, output in out\build_switch.log ...
call :build %* > out\build_switch.log 2>&1
if errorlevel 1 (
  echo [switch] FAILED - see out\build_switch.log
  exit /b 1
)
if defined PURE_BUILD (echo [switch] done: %PURE_BUILD%\pure.nro) else (echo [switch] done: out\nx\pure.nro)
echo [switch] next: package_switch.cmd
exit /b 0

:build
rem --- 1. PATH without MSYS2 / MinGW / devkitPro tools (their cmd, link, find and cmake break CMake)
for /f "usebackq delims=" %%p in (`powershell -NoProfile -Command "($env:PATH -split ';' | Where-Object { $_ -and $_ -notmatch 'devkitpro|msys|mingw' }) -join ';'"`) do set "PATH=%%p"

rem --- 2. devkitPro
if not defined DEVKITPRO_WIN set "DEVKITPRO_WIN=C:\devkitPro"
set "DKP=%DEVKITPRO_WIN:\=/%"
if not exist "%DEVKITPRO_WIN%\devkitA64\bin\aarch64-none-elf-g++.exe" (echo [switch] devkitA64 not found in "%DEVKITPRO_WIN%" & exit /b 1)
if not exist "%DEVKITPRO_WIN%\libnx\switch.specs" (echo [switch] libnx not found in "%DEVKITPRO_WIN%\libnx" & exit /b 1)
if not exist "%DEVKITPRO_WIN%\tools\bin\elf2nro.exe" (echo [switch] elf2nro not found in "%DEVKITPRO_WIN%\tools\bin" & exit /b 1)
for %%l in (zstd z) do if not exist "%DEVKITPRO_WIN%\portlibs\switch\lib\lib%%l.a" (echo [switch] lib%%l.a not found in "%DEVKITPRO_WIN%\portlibs\switch\lib" & exit /b 1)
rem The toolchain reads DEVKITPRO; devkitPro's installer sets it to an MSYS2 path (/opt/devkitpro) CMake cannot open.
set "DEVKITPRO=%DKP%"

rem --- 3. CMake and Ninja from Visual Studio
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -property installationPath`) do set "VS=%%i"
set "VSCMAKE=%VS%\Common7\IDE\CommonExtensions\Microsoft\CMake"
if not exist "%VSCMAKE%\CMake\bin\cmake.exe" (echo [switch] Visual Studio's CMake not found ^("C++ CMake tools for Windows"^) & exit /b 1)
set "PATH=%VSCMAKE%\CMake\bin;%VSCMAKE%\Ninja;%PATH%"

rem --- 4. The SDK, the toolchain file, the generated code
if not defined REXSDK_DIR set "REXSDK_DIR=%~dp0tools\rexglue-sdk-pure\sdk"
for %%d in ("%REXSDK_DIR%") do set "REXSDK_DIR=%%~fd"
if not defined SWITCH_TOOLCHAIN set "SWITCH_TOOLCHAIN=%~dp0..\sa2-nx\tools\switch\cmake\switch-devkitA64.cmake"
for %%d in ("%SWITCH_TOOLCHAIN%") do set "SWITCH_TOOLCHAIN=%%~fd"
if not exist "%REXSDK_DIR%\CMakeLists.txt" (echo [switch] SDK not found at "%REXSDK_DIR%" & exit /b 1)
if not exist "%SWITCH_TOOLCHAIN%" (echo [switch] no "%SWITCH_TOOLCHAIN%" & exit /b 1)
if not exist "generated\default\sources.cmake" (echo [switch] generated\default is missing: run codegen.cmd first & exit /b 1)
if not exist "%~dp0..\sa2-nx\app\thirdparty\plume\CMakeLists.txt" (echo [switch] no ..\sa2-nx\app\thirdparty\plume: Renut's third-party code & exit /b 1)
if not exist "switch_shaders\deko3d_glsl.bin" (echo [switch] switch_shaders\ is missing & exit /b 1)
rem Renut's build wants the shader containers it was made from (renut_shaders\, from renut_shaders.zip).
if not exist "renut_shaders\" (
  if not exist "renut_shaders.zip" (echo [switch] no renut_shaders\ and no renut_shaders.zip & exit /b 1)
  powershell -NoProfile -Command "Expand-Archive -Force 'renut_shaders.zip' 'renut_shaders'" || exit /b 1
)

rem --- 5. No Vulkan driver in this NRO: the SDK only needs Vulkan's headers and the name of a driver archive, which is
rem     never linked (REXGLUE_SWITCH_NO_NVK). out\nvk-stub stands in for Mesa's Switch SDK (as dishonored-nx).
set "NVK=%~dp0out\nvk-stub"
if not exist "%NVK%\lib\libvulkan.a" (
  if not exist "%REXSDK_DIR%\thirdparty\vulkan-headers\include\vulkan\vulkan.h" (echo [switch] no Vulkan headers in the SDK & exit /b 1)
  mkdir "%NVK%\lib" 2>nul
  xcopy /e /i /q /y "%REXSDK_DIR%\thirdparty\vulkan-headers\include" "%NVK%\include" >nul || exit /b 1
  powershell -NoProfile -Command "[IO.File]::WriteAllBytes('%NVK%\lib\libvulkan.a', [byte[]](33,60,97,114,99,104,62,10))" || exit /b 1
)
set "NVK_FWD=%NVK:\=/%"

rem --- 6. Configure then build. Re-run CMake each time so changed toolchain/options
rem are applied. Never recursively delete BUILD: PURE_BUILD can point outside this repo.
if not defined JOBS set "JOBS=4"
rem pure-nx: build_switch_pgo.cmd uses its own PURE_BUILD and PURE_CMAKE_EXTRA.
if defined PURE_BUILD (set "BUILD=%PURE_BUILD%") else (set "BUILD=out\nx")
cmake -S . -B "%BUILD%" -G Ninja -DCMAKE_BUILD_TYPE=Release ^
  "-DCMAKE_TOOLCHAIN_FILE=%SWITCH_TOOLCHAIN%" ^
  "-DCMAKE_MAKE_PROGRAM=%VSCMAKE%\Ninja\ninja.exe" ^
  "-DDEVKITPRO=%DKP%" "-DREXSDK_DIR=%REXSDK_DIR%" ^
  "-DREXGLUE_SWITCH_NVK_SDK=%NVK_FWD%" -DREXGLUE_SWITCH_NO_NVK=ON %PURE_CMAKE_EXTRA% || (echo [switch] CMake configure failed & exit /b 1)
cmake --build "%BUILD%" -j %JOBS% %* || exit /b 1
exit /b 0
