@echo off
rem Builds Pure for the PC (reference build, emulated Xenos GPU): out\build\win-amd64-relwithdebinfo\pure.exe
rem Run from any cmd window: it sets up its own PATH (dishonored-nx's build.cmd method). The whole output goes to
rem out\build_pc.log. Does not touch generated\ (the code generator only reruns if its inputs changed).
setlocal EnableDelayedExpansion
cd /d "%~dp0"
if not exist out mkdir out
echo [build_pc] building, output in out\build_pc.log ...
call :build > out\build_pc.log 2>&1
if errorlevel 1 (
  echo [build_pc] FAILED - see out\build_pc.log
  exit /b 1
)
echo [build_pc] done: out\build\win-amd64-relwithdebinfo\pure.exe
exit /b 0

:build
rem --- 1. PATH without devkitPro / MSYS2 / MinGW: their cmake sees /x/... paths and disables the Windows preset
for /f "usebackq delims=" %%p in (`powershell -NoProfile -Command "($env:PATH -split ';' | Where-Object { $_ -and $_ -notmatch 'devkitpro|msys|mingw' }) -join ';'"`) do set "PATH=%%p"

rem --- 2. Visual Studio environment (MSVC libraries + Windows SDK), 64-bit, with its CMake and Ninja
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS=%%i"
if not defined VS (echo [build_pc] Visual Studio with the C++ tools not found & exit /b 1)
call "%VS%\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul || exit /b 1
set "VSCMAKE=%VS%\Common7\IDE\CommonExtensions\Microsoft\CMake"
if not exist "%VSCMAKE%\CMake\bin\cmake.exe" (echo [build_pc] "C++ CMake tools for Windows" missing in Visual Studio & exit /b 1)

rem --- 3. LLVM clang (the preset names the compiler as plain clang / clang++)
if not defined LLVM_DIR set "LLVM_DIR=%ProgramFiles%\LLVM"
if not exist "%LLVM_DIR%\bin\clang++.exe" (echo [build_pc] clang++ not found in "%LLVM_DIR%\bin" & exit /b 1)
set "PATH=%LLVM_DIR%\bin;%VSCMAKE%\CMake\bin;%VSCMAKE%\Ninja;%PATH%"
echo [build_pc] cmake: & where cmake
echo [build_pc] clang: & clang++ --version

rem --- 4. Configure (preset win-amd64-relwithdebinfo, quick when nothing changed), then build
if not defined JOBS set "JOBS=6"
rem CMAKE_MAKE_PROGRAM explicitly: an earlier configure with devkitPro's MSYS2 on PATH cached its ninja, which runs
rem commands through /bin/sh and strips the backslashes of Windows paths.
cmake --preset win-amd64-relwithdebinfo "-DCMAKE_MAKE_PROGRAM=%VSCMAKE%\Ninja\ninja.exe" || exit /b 1
cmake --build out\build\win-amd64-relwithdebinfo -j %JOBS% || exit /b 1
exit /b 0
