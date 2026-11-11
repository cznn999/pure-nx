@echo off
rem pure-nx: first-time setup of a fresh clone, then codegen.
rem   1. the SDK's third-party sources (tools\rexglue-sdk-pure\tools\fetch_thirdparty.py; needs git and Python 3)
rem   2. the ReXGlue code generator, rexglue.exe (clang, Visual Studio's CMake and Ninja)
rem   3. codegen.cmd: your game (game_root\default.xex, see pure_manifest.toml) into generated\default
rem Then put renut_shaders\ and switch_shaders\ (made from your game, see README.md) here and run build_switch.cmd.
setlocal
cd /d "%~dp0"
set "SDKROOT=%~dp0tools\rexglue-sdk-pure"
if not exist "game_root\default.xex" (echo [setup] put your extracted game in game_root\ ^(default.xex^) first & exit /b 1)

if not exist "%SDKROOT%\sdk\thirdparty\fmt" (
  echo [setup] fetching the SDK's third-party sources ...
  python "%SDKROOT%\tools\fetch_thirdparty.py" || (echo [setup] fetch_thirdparty.py failed & exit /b 1)
)

if exist "%SDKROOT%\sdk\out\host\rexglue.exe" goto codegen
if exist "%SDKROOT%\sdk\out\win-amd64\rexglue.exe" goto codegen
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -property installationPath`) do set "VS=%%i"
if not exist "%VS%\VC\Auxiliary\Build\vcvars64.bat" (echo [setup] Visual Studio with C++ not found & exit /b 1)
for /f "usebackq delims=" %%p in (`powershell -NoProfile -Command "($env:PATH -split ';' | Where-Object { $_ -and $_ -notmatch 'devkitpro|msys|mingw' }) -join ';'"`) do set "PATH=%%p"
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
rem Visual Studio's CMake and Ninja (vcvars64 does not always add them).
set "VSCMAKE=%VS%\Common7\IDE\CommonExtensions\Microsoft\CMake"
if not exist "%VSCMAKE%\CMake\bin\cmake.exe" (echo [setup] install "C++ CMake tools for Windows" in Visual Studio & exit /b 1)
set "PATH=%VSCMAKE%\CMake\bin;%VSCMAKE%\Ninja;%PATH%"
rem The SDK builds with clang/clang++ (GNU command line, as its CMakePresets.json), not clang-cl: LLVM's own install
rem first, else Visual Studio's ("C++ Clang tools for Windows").
set "CLANGDIR="
for %%d in ("%ProgramFiles%\LLVM\bin" "%VS%\VC\Tools\Llvm\x64\bin") do if not defined CLANGDIR if exist "%%~d\clang.exe" set "CLANGDIR=%%~d"
if not defined CLANGDIR (echo [setup] clang not found: install LLVM or Visual Studio's "C++ Clang tools for Windows" & exit /b 1)
set "CLANGXX=%CLANGDIR%\clang++.exe"
if not exist "%CLANGXX%" set "CLANGXX=%CLANGDIR%\clang.exe"
set "CLANGC=%CLANGDIR:\=/%/clang.exe"
set "CLANGXX=%CLANGXX:\=/%"
echo [setup] building rexglue.exe with %CLANGXX% ...
cmake -S "%SDKROOT%\sdk" -B "%SDKROOT%\sdk\out\host" -G Ninja -DCMAKE_BUILD_TYPE=Release ^
  "-DCMAKE_C_COMPILER=%CLANGC%" "-DCMAKE_CXX_COMPILER=%CLANGXX%" -DCMAKE_CXX_STANDARD=23 ^
  "-DCMAKE_C_FLAGS=-march=x86-64-v2" "-DCMAKE_CXX_FLAGS=-march=x86-64-v2" || (echo [setup] SDK configure failed & exit /b 1)
cmake --build "%SDKROOT%\sdk\out\host" --target rexglue || (echo [setup] rexglue build failed & exit /b 1)
rem The SDK puts rexglue.exe in sdk\out\win-amd64 (its CMakeLists.txt), whatever the build folder.
if not exist "%SDKROOT%\sdk\out\win-amd64\rexglue.exe" if not exist "%SDKROOT%\sdk\out\host\rexglue.exe" (echo [setup] rexglue.exe not found in sdk\out & exit /b 1)

:codegen
call codegen.cmd || exit /b 1
if not exist renut_shaders (echo [setup] done; still needed: renut_shaders\ and switch_shaders\ ^(README.md^) & exit /b 0)
echo [setup] done: run build_switch.cmd
