@echo off
rem Runs the headless UI harness, then builds and opens demo\prometheus99_demo.html.
rem Needs the MSYS2 UCRT64 tools (mingw32-make, gcc) and Python on PATH.
cd /d "%~dp0.."
mingw32-make ui-host
if errorlevel 1 (
  echo The build or the scenario failed. See the messages above.
  pause
  exit /b 1
)
python demo\make_demo.py
if errorlevel 1 (
  pause
  exit /b 1
)
start "" "demo\prometheus99_demo.html"
