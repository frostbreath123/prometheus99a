@echo off
setlocal enabledelayedexpansion
echo =================================================================
echo   Building Prometheus99: Lightweight HMI Runtime (C99 + LVGL)
echo =================================================================

REM Check if prometheus99.exe is running and stop it to allow overwriting
taskkill /F /IM prometheus99.exe >nul 2>&1

REM 1. Try Microsoft Visual C++ compiler (MSVC) first
set "VCVARS="
if exist "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat" (
    set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
)

if defined VCVARS (
    echo [BUILD] Using MSVC Compiler with Size and RAM Optimization...
    call "%VCVARS%" >nul 2>&1
    cl.exe /nologo /O1 /Os /GL /Gy /GF /Gw /GS- /MD /D_CRT_SECURE_NO_WARNINGS /DNDEBUG /Iinclude ^
        src\layer0_hardware.c ^
        src\layer1_hal.c ^
        src\layer2_core.c ^
        src\layer3_presentation.c ^
        src\main.c ^
        /Fe:prometheus99.exe ^
        /link /OPT:REF /OPT:ICF /LTCG /FIXED /DEBUG:NONE /MERGE:.pdata=.text /STACK:32768,4096 /HEAP:65536,4096 user32.lib gdi32.lib winmm.lib psapi.lib >nul 2>&1
    
    if !ERRORLEVEL! EQU 0 (
        del /f /q *.obj >nul 2>&1
        goto :build_success
    )
    echo [WARN] MSVC build failed, attempting GCC fallback...
)

REM 2. Fallback: MinGW / GCC
set "WINLIBS_PATH=%LOCALAPPDATA%\Microsoft\WinGet\Packages\BrechtSanders.WinLibs.POSIX.UCRT_Microsoft.Winget.Source_8wekyb3d8bbwe\mingw64\bin"
set PATH=%WINLIBS_PATH%;C:\msys64\mingw64\bin;%PATH%

echo [BUILD] Using GCC with Size Optimization...
gcc -std=c99 -Wall -Wextra -Wpedantic -Os -DNDEBUG -Iinclude ^
    -ffunction-sections -fdata-sections ^
    src/layer0_hardware.c ^
    src/layer1_hal.c ^
    src/layer2_core.c ^
    src/layer3_presentation.c ^
    src/main.c ^
    -o prometheus99.exe ^
    -lgdi32 -luser32 -lwinmm -lpsapi ^
    -Wl,--gc-sections -Wl,--stack,65536 -s

if %ERRORLEVEL% NEQ 0 (
    echo [ERROR] Compilation failed with error code %ERRORLEVEL%
    exit /b %ERRORLEVEL%
)

:build_success
echo [SUCCESS] Build succeeded!
for %%A in (prometheus99.exe) do echo [SIZE] Binary size: %%~zA bytes
echo =================================================================
exit /b 0
