@echo off
rem Builds tpw-rtx into build\  (see docs\BUILDING.md)
rem Needs: Visual Studio 2022 with "Desktop development with C++" (includes the Windows SDK and dxc),
rem        the NVIDIA DLSS SDK in third_party\DLSS (or set DLSS_SDK to its folder).
setlocal
cd /d "%~dp0"
if not defined DLSS_SDK set "DLSS_SDK=%~dp0third_party\DLSS"
if not exist "%DLSS_SDK%\include\nvsdk_ngx.h" (
    echo NVIDIA DLSS SDK not found in "%DLSS_SDK%". Get it with:
    echo     git clone --depth 1 --branch v310.9.16 https://github.com/NVIDIA/DLSS third_party\DLSS
    exit /b 1
)
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS=%%i"
if not defined VS ( echo Visual Studio 2022 with C++ tools not found. & exit /b 1 )
if not exist build mkdir build

echo [1/3] hook: dinput.dll (32-bit, loaded by TP.EXE)
setlocal
call "%VS%\VC\Auxiliary\Build\vcvars32.bat" >nul || exit /b 1
cl /nologo /O2 /W3 /MT /LD /EHsc /D_CRT_SECURE_NO_WARNINGS src\hook\tphook.c src\hook\overlay.cpp /Fobuild\ /Fe:build\dinput.dll ^
   /link /DEF:src\hook\dinput.def /NOIMPLIB /NOEXP user32.lib winmm.lib d3d11.lib dxgi.lib dcomp.lib || exit /b 1
endlocal

echo [2/3] renderer: tpwrtx_viewer.exe + rt.cso (64-bit, D3D12 + DXR + DLSS Ray Reconstruction)
setlocal
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set "DXC=dxc.exe"
where dxc.exe >nul 2>nul || set "DXC=%ProgramFiles(x86)%\Windows Kits\10\Redist\D3D\x64\dxc.exe"
"%DXC%" -nologo -T cs_6_5 -E main -O3 -HV 2018 src\viewer\rt.hlsl -Fo build\rt.cso || exit /b 1
cl /nologo /O2 /W3 /EHsc /std:c++17 /MT /D_CRT_SECURE_NO_WARNINGS /I "%DLSS_SDK%\include" src\viewer\tpwrtx_viewer.cpp /Fobuild\ /Fe:build\tpwrtx_viewer.exe ^
   /link /SUBSYSTEM:WINDOWS /MANIFEST:EMBED user32.lib advapi32.lib "%DLSS_SDK%\lib\Windows_x86_64\x64\nvsdk_ngx_s.lib" || exit /b 1

echo [3/3] optional, unofficial: DLSS 5 forwarder (see docs\DLSS5.md)
cl /nologo /O2 /W3 /LD src\dlss5\nr_forwarder.c /Fobuild\ /Fe:build\tpwrtx_nvngx.dll_forwarder.dll /link /NOIMPLIB /NOEXP || exit /b 1
endlocal

copy /y config\tpwrtx.ini build\ >nul && copy /y config\dxwrapper.ini build\ >nul && copy /y install.bat build\ >nul
del /q build\*.obj 2>nul
echo.
echo Done. Copy everything in build\ into your game folder, or run build\install.bat "<game folder>".
