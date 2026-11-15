@echo off
rem tpw-rtx installer: copies the files next to this script into your SimTheme Park folder.
rem Usage: install.bat "C:\Games\SimTheme Park"      (or run it and type the folder)
setlocal
cd /d "%~dp0"
set "GAME=%~1"
if "%GAME%"=="" set /p "GAME=SimTheme Park folder (the one containing TP.EXE): "
set "GAME=%GAME:"=%"
if not exist "%GAME%\TP.EXE" echo TP.EXE not found in "%GAME%". & exit /b 1

rem Ray tracing needs the exact TP.EXE build the hook was made for; anything else gets the fixes only.
set "HASH="
for /f "skip=1 delims=" %%h in ('certutil -hashfile "%GAME%\TP.EXE" SHA256 ^| findstr /v /c:"CertUtil"') do if not defined HASH set "HASH=%%h"
set "HASH=%HASH: =%"
if /i "%HASH%"=="d0399acfc1bcbdcbe5836814b19e386486bfa06f057646ae7821bc25e9927ac9" echo TP.EXE: supported build.
if /i not "%HASH%"=="d0399acfc1bcbdcbe5836814b19e386486bfa06f057646ae7821bc25e9927ac9" echo WARNING: this TP.EXE is a different build. Only the speed, video-memory and alt-tab fixes will work, not the ray tracing.

set "BK=%GAME%\tpw-rtx-backup"
if not exist "%BK%" mkdir "%BK%" 2>nul
if not exist "%BK%" echo Cannot write to "%GAME%". Copy the game to a folder outside Program Files first, e.g. C:\Games\SimTheme Park. & exit /b 1
for %%f in (dinput.dll tpwrtx_viewer.exe rt.cso tpwrtx_nvngx.dll_forwarder.dll tpwrtx.ini dxwrapper.ini) do (
    if exist "%GAME%\%%f" if not exist "%BK%\%%f" copy /y "%GAME%\%%f" "%BK%\" >nul
    copy /y "%%f" "%GAME%\" >nul || ( echo Failed to copy %%f & exit /b 1 )
)
echo tpw-rtx files installed. Anything they replaced is in "%BK%".
echo.
set "OK=1"
if not exist "%GAME%\dxwrapper.dll" set "OK=" & echo STILL NEEDED: dxwrapper.dll from dxwrapper.zip, https://github.com/elishacloud/dxwrapper/releases/tag/v1.4.7900.25
if not exist "%GAME%\ddraw.dll" set "OK=" & echo STILL NEEDED: ddraw.dll from the Stub folder of that same dxwrapper.zip
if not exist "%GAME%\nvngx_dlssd.dll" echo optional: nvngx_dlssd.dll for DLSS Ray Reconstruction, https://github.com/NVIDIA/DLSS/raw/v310.9.16/lib/Windows_x86_64/rel/nvngx_dlssd.dll
if not exist "%GAME%\nvngx_dlssnr.dll" echo optional, unofficial: nvngx_dlssnr.dll for DLSS 5, see docs\DLSS5.md
echo.
if defined OK echo Ready: start TP.EXE (as administrator if it closes right away), open a park, press Ctrl+Alt+S for settings.
if not defined OK echo Copy the files listed above into "%GAME%", then start TP.EXE.
