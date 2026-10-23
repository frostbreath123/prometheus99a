# Demo of the Prometheus99 HMI

Two ways to show the HMI to other people. Both are demonstrations, not the product.

## 1. Live window (Windows)

Double-click `demo/run_live.bat`, or run `mingw32-make demo-live`. It builds `build/hmi_win_demo.exe` and
opens a window of 800 by 480 pixels. A mouse click stands in for a touch.

- The real LVGL screens, the real runtime and the real libmodbus driver run against the test fake PLC
  (`tests/hmi/fake_modbus_server.c`). The values move, so trends, alarms and quality colours show something.
- There is no login. Press F1 for the help, F3 to change the claimed role (viewer, operator, engineer, admin), or start the
  program with `--role admin` and so on. The default is operator.
- F2 cuts or restores the PLC link. F3 changes the role. Esc closes.
- Things to try: TRENDS, the alarm list, a setpoint above 80 on TANK_LEVEL as the operator,
  SETTINGS then CONFIG as the engineer (APPLY needs two presses).

What is different from the real device:
- The PLC is a fake. Nothing real is connected.
- The configuration is not signed. A small stand-in for the root helper `p99_cfg` runs inside the program.
  It applies a change request from the CONFIG screen with the same rules, writes the audit line and the
  result, then restarts the runtime with the new configuration. The role stays; the trend starts again.
- The window is drawn with plain Win32 GDI. The real device draws on a Linux framebuffer.
- Nothing here was measured on a Raspberry Pi or shown on a real display.

## 2. Slideshow (any computer, offline)

Double-click `demo/run_demo.bat`. It runs the headless harness and builds `demo/prometheus99_demo.html`, one
file with 20 captioned screens that opens in any browser. Send that file to people who cannot run the program.
