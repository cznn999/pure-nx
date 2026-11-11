@echo off
rem pure-nx: reruns the ReXGlue code generator on pure_manifest.toml (+ overrides.toml) into generated\default.
rem Only files whose content changed are written. Output in out\codegen.log. Takes several minutes.
setlocal
cd /d "%~dp0"
for /f "usebackq delims=" %%p in (`powershell -NoProfile -Command "($env:PATH -split ';' | Where-Object { $_ -and $_ -notmatch 'devkitpro|msys|mingw' }) -join ';'"`) do set "PATH=%%p"
set "REXGLUE=%~dp0tools\rexglue-sdk-pure\sdk\out\win-amd64\rexgluerd.exe"
if not exist "%REXGLUE%" set "REXGLUE=%~dp0tools\rexglue-sdk-pure\sdk\out\win-amd64\rexglue.exe"
if not exist "%REXGLUE%" set "REXGLUE=%~dp0tools\rexglue-sdk-pure\sdk\out\host\rexglue.exe"
if not exist "%REXGLUE%" (echo [codegen] no rexglue.exe: build it first, see README.md "Building" & exit /b 1)
if not exist out mkdir out
echo [codegen] %REXGLUE% codegen pure_manifest.toml ...
rem The loaded game image for tools\post_codegen_pure.py (constants, entry point).
set "REXGLUE_DUMP_IMAGE=%~dp0out\default_image.bin"
"%REXGLUE%" --force codegen pure_manifest.toml > out\codegen.log 2>&1
if errorlevel 1 (echo [codegen] FAILED - see out\codegen.log & exit /b 1)
rem The optimisations the released NRO is built with (tools\post_codegen_pure.py); required after every codegen.
echo [codegen] post-processing ...
python tools\post_codegen_pure.py >> out\codegen.log 2>&1
if errorlevel 1 (echo [codegen] post-processing FAILED - see out\codegen.log & exit /b 1)
echo [codegen] done - see out\codegen.log
