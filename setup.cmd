@echo off
rem pure-nx: first-time setup of a fresh clone, then codegen.
rem   1. the SDK's third-party sources (tools\rexglue-sdk-pure\tools\fetch_thirdparty.py; needs git and Python 3)
rem   2. the ReXGlue code generator, rexglue.exe (Visual Studio's clang-cl, CMake and Ninja)
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

rem clang-cl may be installed with LLVM separately from Visual Studio; vcvars64 does not
rem reliably add that installation to PATH. Try the standard LLVM locations if needed.
where clang-cl >nul 2>nul
if errorlevel 1 (
  if exist "%ProgramFiles%\LLVM\bin\clang-cl.exe" set "PATH=%ProgramFiles%\LLVM\bin;%PATH%"
  if exist "%ProgramFiles(x86)%\LLVM\bin\clang-cl.exe" set "PATH=%ProgramFiles(x86)%\LLVM\bin;%PATH%"
)
where clang-cl >nul 2>nul || (echo [setup] clang-cl not found; install LLVM Clang 18+ and add its bin directory to PATH & exit /b 1)
for /f "delims=" %%c in ('where clang-cl') do if not defined CLANGCL set "CLANGCL=%%c"
echo [setup] using %CLANGCL%
echo [setup] building rexglue.exe ...
cmake -S "%SDKROOT%\sdk" -B "%SDKROOT%\sdk\out\host" -G Ninja -DCMAKE_BUILD_TYPE=Release "-DCMAKE_C_COMPILER=%CLANGCL%" "-DCMAKE_CXX_COMPILER=%CLANGCL%" || (echo [setup] SDK configure failed & exit /b 1)

:codegen
call codegen.cmd || exit /b 1
if not exist renut_shaders (echo [setup] done; still needed: renut_shaders\ and switch_shaders\ ^(README.md^) & exit /b 0)
echo [setup] done: run build_switch.cmd
