@echo off
setlocal
if /I not "%VSCMD_ARG_TGT_ARCH%"=="x64" (
    echo Open an x64 Native Tools Command Prompt for Visual Studio, then run build.cmd.
    exit /b 1
)

pushd "%~dp0" || exit /b 1
if not exist "build" mkdir "build"
cl /nologo /O2 /MT /EHsc /W4 /WX /std:c++17 /DUNICODE /D_UNICODE /DNOMINMAX /D_WIN32_WINNT=0x0A00 ^
    NativeOverlay.cpp /Fo:"build\NativeOverlay.obj" /Fe:"build\AION2 Network Overlay.exe" ^
    /link /SUBSYSTEM:WINDOWS /MANIFEST:EMBED /MANIFESTUAC:NO /MANIFESTINPUT:"NativeOverlay.manifest" ^
    iphlpapi.lib advapi32.lib ws2_32.lib user32.lib gdi32.lib ole32.lib comctl32.lib
set "OverlayBuildExit=%errorlevel%"
popd
exit /b %OverlayBuildExit%
