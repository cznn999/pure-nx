@echo off
rem pure-nx (2026-11-07): profile-guided and link-time optimisation of the Switch build (renut-nx's docs/pgo-lto.md,
rem sa2-nx's method). The same source, compiled with what one console session counted: nothing it computes changes.
rem
rem   build_switch_pgo.cmd generate   instrumented NRO: out\nx_pgo_gen\pure.nro. Put it on the SD as
rem                                   /switch/pure-nx/pure.nro and play (menus, then races with bots: 10 min or more);
rem                                   it writes /switch/pure-nx/pgo/pure_pgo_<N>.stream every minute.
rem   python tools\pgo_merge.py <the SD's pgo folder copied to the PC>
rem                                   merges every session into pgo\ (.gcda files; move an old pgo\ aside first).
rem   build_switch_pgo.cmd use [keep] the optimised NRO with LTO: out\nx_pgo_use\pure.nro (a long link: set JOBS=2
rem                                   and PURE_LTO_JOBS=1 with little memory).
rem
rem The instrumented NRO runs slower (it counts every branch); only the optimised one is for playing. A change to the
rem game code or the renderer after the session keeps the counts of every function it did not change.
setlocal
cd /d "%~dp0"
if /i "%~1"=="generate" (
  set "PURE_BUILD=out\nx_pgo_gen"
  set "PURE_CMAKE_EXTRA=-DPURE_PGO=generate"
) else if /i "%~1"=="use" (
  if not exist "pgo\" (echo [pgo] no pgo\ folder: run python tools\pgo_merge.py first & exit /b 1)
  if not defined PURE_LTO_JOBS set "PURE_LTO_JOBS=2"
  set "PURE_BUILD=out\nx_pgo_use"
) else (
  echo usage: build_switch_pgo.cmd generate ^| use
  exit /b 1
)
if /i "%~1"=="use" set "PURE_CMAKE_EXTRA=-DPURE_PGO=use -DPURE_SWITCH_LTO=ON -DPURE_SWITCH_LTO_JOBS=%PURE_LTO_JOBS%"
rem ninja does not see the .gcda files as inputs: a new profile needs a clean build of the optimised folder.
rem pure-nx (2026-11-08): "use keep" builds again with the same profile (a change outside the generated code, the
rem profile untouched): no clean, only what changed is compiled again.
if /i "%~1"=="use" if /i not "%~2"=="keep" if exist "out\nx_pgo_use\build.ninja" (
  echo [pgo] cleaning out\nx_pgo_use for the new profile
  rmdir /s /q "out\nx_pgo_use"
)
call build_switch.cmd
exit /b %errorlevel%
