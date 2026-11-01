# Prometheus99

Prometheus99 is a small industrial HMI (human-machine interface) runtime written in C99. It reads a PLC over Modbus TCP and shows live values, alarms and trends on a touch screen. A permitted operator can acknowledge alarms and write set-points. It is built for a Raspberry Pi or a similar Linux board with a framebuffer display. The device program is one static file of about 318 KB (350 KB for 64-bit ARM) that uses about 0.6 MB of memory.

Authors: Team Doomsday (Abhilash L, Adarsh Abhilash, Nikhil Nuguri). Licence: MIT (`LICENSE`). Third-party components and their licences: `docs/THIRD_PARTY.md`.

**Status: a working, presentation-ready prototype, not a production product.** It was run on a development PC (Windows 11, Ubuntu 24.04 under WSL2) and in an emulated ARM computer, always against a simulated PLC. It has not run on a Raspberry Pi, a real touch panel, a real PLC or real systemd. The full results, measurements, failures found and remaining limits are in `docs/FINAL_REPORT.md`.

## Try it

| What | How |
|---|---|
| **Windows** (nothing to build) | Double-click `bin\windows\Prometheus99_Demo.exe`. An 800 x 480 window opens with the real interface and a built-in simulated PLC. Click like a touch screen. F1 help, F2 cut or restore the PLC link, F3 change the claimed role, Esc closes. |
| **Linux window** | `sudo apt install build-essential pkg-config libmodbus-dev libsdl2-dev`, then `bash demo/run_linux_sdl.sh`. Needs a window system (WSLg on Windows 11, or X11). |
| **Raspberry Pi target in an emulator** | `bash emulation/pi/prepare.sh`, `bash emulation/pi/make_initramfs.sh`, `bash emulation/pi/run_qemu.sh window`. It boots an emulated ARM computer (Cortex-A53, virtual display and touch) and runs the aarch64 program. See `docs/EMULATION.md` for the prerequisites and for what this proves and does not prove. |
| **Screenshots of every target** | `evidence/` holds pictures taken from the running programs: the Windows demo, the Linux window and the emulated Pi, each through the same tour (normal operation, high alarm, acknowledge, setpoint write, trends, settings, PLC cut, recovery, viewer role). |

Prebuilt programs are in `bin/` with checksums in `bin/SHA256SUMS`: `windows/Prometheus99_Demo.exe`, `linux-x86_64/prometheus99` and `p99_sign`, `linux-aarch64/prometheus99` and `p99_sign` (static, no shared libraries).

## What it does

- **Modbus TCP polling** through libmodbus, with typed tags (16 and 32 bit integers and 32 bit floats, word and byte order per tag, scale), a quality value for every tag, bounded timeouts, and reconnection with back-off. A connected socket with a silent PLC is reported as no response.
- **Alarms** with four limits, hysteresis, delays, acknowledgement, escalation, return to normal, and a communication-loss alarm. The alarm journal and state are kept on disk.
- **Writes** are validated (writes enabled, tag allowed, role, range, rate limit), checked again just before sending, sent at most once, then read back and verified. A lost reply is marked unknown and never retried. If the audit record cannot be stored, the write is refused.
- **Signed configuration** (HMAC-SHA-512, rollback serial, PLC allow-list). An unsigned, altered or older configuration puts the device in a safe mode with no PLC connection and writes off.
- **Audit trail and storage** through one worker thread, so the screens never wait for the disk.
- **Screens** (LVGL 9.2.2): overview, alarms, trends, settings, a number pad for set-points, and an on-device configuration editor whose requests are applied by a root helper. The helper is the same program started under the name `p99_cfg`.
- **Display**: the program draws RGB565 and writes to a 16 or 32 bits per pixel framebuffer (a default Raspberry Pi framebuffer works), or runs without a display with `--fb none`.

## Results at a glance (host and emulator figures)

| | Windows demo | Linux x86-64 | aarch64 (emulated) |
|---|---|---|---|
| Program size, stripped | 304,640 bytes | 317,616 bytes | 350,288 bytes |
| Program memory | 3.5 MB (an empty Windows window uses 1.4 MB) | 0.55 to 0.61 MB | 0.63 to 0.68 MB |
| Continuous run | 28 s tour | 1 hour, 11 injected PLC faults, no crash | 42 minutes, 7 faults, no crash |
| Tests | 3,253 core, 208 view, 205 UI, 75 end-to-end | 3,275 core, 208 view, 205 UI, 75 end-to-end, executable tests, 150-round kill-loop | same program, scripted tour |

Display memory (the framebuffer, owned by the kernel) is separate: 0.75 MB at 16 bits per pixel and 1.5 MB at 32. The program plus the display memory is about 1.3 MB, so "below 1 MB" holds for the program, not for the program plus the framebuffer.

## Layout

```
README.md, LICENSE   this file and the MIT licence
bin/                 prebuilt programs for Windows, Linux x86-64 and aarch64, and SHA256SUMS
evidence/            screenshots and logs from the running programs (windows, linux, pi_emulated)
src/hmi/             portable core: tags, comms, writes, alarms, config, storage, runtime
src/ui/              LVGL screens, theme and the single embedded font
src/app/             main_linux.c, display_*.c (framebuffer, file, SDL backends), p99_sign.c, p99_cfg.c
config/              hmi.example.json, lv_conf.h
deploy/              systemd units
demo/                Windows demo source, tour scripts, run scripts
emulation/pi/        the emulated Raspberry Pi-class target (QEMU)
tests/hmi/           unit, end-to-end, UI and executable tests, and the fake PLC
third_party/         LVGL, libmodbus, Monocypher, jsmn (only what the build compiles) with licences
docs/                FINAL_REPORT, EMULATION, THIRD_PARTY, SCREENS, CONFIG_INTEGRITY, STORAGE, AUTHENTICATION
sources.mk, Makefile.linux, Makefile.windows   build files; the source lists are in sources.mk
```

## Build, configure, run (device program)

On Debian, Ubuntu or Raspberry Pi OS: `sudo apt install build-essential pkg-config libmodbus-dev python3 musl-tools`.

```sh
# the small static device program (build directory build-musl)
make -f Makefile.linux STATIC=1 CC=musl-gcc BUILD=build-musl hmi
# for aarch64: use an aarch64 musl compiler, for example
make -f Makefile.linux STATIC=1 CC=aarch64-linux-musl-gcc BUILD=build-aarch64 hmi
```

1. Copy `config/hmi.example.json` to `hmi.json` and set `plc.host`, `plc.allowed_hosts` and the tags. The example uses a LAN address.
2. Sign it: `p99_sign keygen --key config.key`, then `p99_sign sign --config hmi.json --key config.key`. Sign again after every change.
3. Run: `prometheus99 --config hmi.json --audit audit.log --alarms alarms.bin --config-key config.key --config-state config.serial [--role viewer|operator|engineer|admin] [--fb /dev/fb0|none] [--touch /dev/input/event0]`. For a service installation see the header of `deploy/prometheus99.service`.

On Windows, build the demo with MSYS2 UCRT64 from a short folder path such as `C:\p99`: `mingw32-make -f Makefile.windows demo-live`. See `demo/README.md`.

## Tests

```sh
make -f Makefile.linux test test-view ui-host e2e cli-exec config-exec cfgedit-exec liveness   # one after another, not with make -j
mingw32-make -f Makefile.windows test-hmi test-view test-config-golden ui-host                 # Windows
```

## Limitations

- **No authentication.** The role is the one given with `--role` when the process starts; anyone who can start it or edit the service unit can choose any role, and the audit trail names no person. An earlier login with a password hash was removed for the memory goal (`docs/AUTHENTICATION.md`). Keep the device physically and administratively controlled. Modbus TCP is not encrypted.
- **Nothing ran on target hardware.** Real display and touch behaviour, the 16 bits per pixel path, readability on a real panel, real timings, real PLC interoperability, real systemd, power loss and long soak tests are unproven. The emulated target is a generic ARM virtual machine with a generic kernel, not Raspberry Pi OS.
- Only Modbus TCP is implemented (no Modbus RTU, no OPC UA).
- No threat model, IEC 62443 mapping, SBOM or legal licence review. The static build puts libmodbus (LGPL-2.1 or later) in the executable, so a delivered device needs the relinking material described in `docs/THIRD_PARTY.md`.
- `docs/CONFIG_INTEGRITY.md` and `docs/STORAGE.md` still contain passages about logins from an earlier design; a note at the top of each says they no longer apply. Some source comments refer to design documents that are not in this folder.
