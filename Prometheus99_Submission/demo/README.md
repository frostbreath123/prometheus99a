# Demos and tour scripts

Everything here is a demonstration or a test aid, not the product. The device program is built on Linux (see the top-level README).

## Windows live demo

1. Install MSYS2 (https://www.msys2.org), open the "MSYS2 UCRT64" shell and install the compiler and make:
   ```sh
   pacman -S --needed mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-make
   ```
2. Put `C:\msys64\ucrt64\bin` on the Windows PATH (or run the next step inside the UCRT64 shell).
3. Double-click `demo\run_live.bat`, or from the repository folder run `mingw32-make demo-live` (or `mingw32-make -f Makefile.windows demo-live`). The first build compiles LVGL and takes a few minutes. Build from a short folder path such as `C:\p99`.

A window of 800 by 480 pixels opens; a mouse click stands in for a touch. The real LVGL screens, the real runtime and the real libmodbus driver run against a fake PLC (`tests/hmi/fake_modbus_server.c`). F1 help, F2 cut or restore the PLC link, F3 change the claimed role (viewer, operator, engineer, admin), Esc closes. `--role admin` and so on set the role at start; `--tour DIR` runs the scripted tour and saves pictures; `--no-help` skips the help box.

Differences from the real device: the PLC is fake; the configuration is not signed (a small stand-in for the root helper `p99_cfg` runs inside the program); the window is drawn with plain Win32 GDI, not a Linux framebuffer; nothing here was measured on a Raspberry Pi.

## Scripted tours (screenshots and measurements)

| Target | Script |
|---|---|
| Windows demo | `powershell -ExecutionPolicy Bypass -File demo\win_capture.ps1 -Exe build-windows\hmi_win_demo.exe -Out evidence\windows` |
| Linux window (SDL2) | `bash demo/linux_tour.sh build-sdl evidence/linux` (needs xdotool and a window system) |
| Emulated Raspberry Pi | `emulation/pi/drive.py`, see `docs/EMULATION.md` |
| One picture of all three | `powershell -ExecutionPolicy Bypass -File demo\make_overview_sheet.ps1` |

`ui_targets.txt` holds the touch-target coordinates shared by the three tour drivers. `make_demo_config.sh` builds the demo configuration from `config/hmi.example.json`. `run_linux_sdl.sh` starts the live Linux window.
