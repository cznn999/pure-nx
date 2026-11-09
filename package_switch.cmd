@echo off
rem pure-nx: the SD card folder of Pure for the Switch: <target>\switch\pure-nx\ with pure.nro and game_root\ (your
rem game files, game_root\ beside this script by default; set GAME=<folder> to change it). <target> is the SD card's root:
rem   package_switch.cmd            -> out\nx\sdcard (copy its "switch" folder to the card)
rem   package_switch.cmd E:\        -> straight onto the SD card in drive E:
rem game_root is copied with robocopy /MIR: only what changed is copied again.
setlocal
cd /d "%~dp0"
if not defined GAME set "GAME=%~dp0game_root"
if not exist "out\nx\pure.nro" (echo [package] no out\nx\pure.nro: run build_switch.cmd first & exit /b 1)
if not exist "%GAME%\default.xex" (echo [package] no %GAME%\default.xex & exit /b 1)
set "TARGET=%~1"
if "%TARGET%"=="" set "TARGET=%~dp0out\nx\sdcard"
set "DEST=%TARGET%\switch\pure-nx"
mkdir "%DEST%" 2>nul
copy /y "out\nx\pure.nro" "%DEST%\pure.nro" >nul || exit /b 1
echo [package] %DEST%\pure.nro
robocopy "%GAME%" "%DEST%\game_root" /MIR /XD "$SystemUpdate" /NFL /NDL /NJH /NP
if errorlevel 8 (echo [package] copying game_root failed & exit /b 1)
echo [package] done: %DEST%
exit /b 0
