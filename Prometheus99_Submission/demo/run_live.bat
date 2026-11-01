@echo off
rem Builds and starts the live demo window (Win32, no extra library).
rem Needs the MSYS2 UCRT64 tools (mingw32-make, gcc) on PATH. The first build compiles LVGL and takes a few minutes.
cd /d "%~dp0.."
mingw32-make build/hmi_win_demo.exe
if errorlevel 1 (
  echo The build failed. See the messages above.
  pause
  exit /b 1
)
cd build
start "" hmi_win_demo.exe
