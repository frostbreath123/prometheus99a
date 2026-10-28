# Emulated Raspberry Pi-class target

This document describes how the aarch64 build of Prometheus99 is run, without a Raspberry Pi, inside an emulated ARM computer with a visible emulated screen. It also states exactly what that proves and what it does not.

## What the emulated target is

| Part | What is used | Same as a Raspberry Pi? |
|---|---|---|
| Machine | QEMU `virt` (aarch64), 2 CPUs, 256 MB RAM | No. It is a generic ARM virtual machine, not a Raspberry Pi board. |
| CPU | emulated Cortex-A53 | Yes, the core of a Pi 3 and a Pi Zero 2 W. It is emulated, so its speed is not the speed of a Pi. |
| Kernel | Alpine Linux 6.18.58 "virt" kernel for aarch64 | No. It is a generic arm64 kernel, not the Raspberry Pi kernel. |
| User space | busybox from Alpine's initramfs, plus the Prometheus99 files | Not Raspberry Pi OS. No systemd. |
| Program | the real static aarch64 musl binary (`build-aarch64/prometheus99`) | Yes: the same file that would be copied to a Pi. |
| Display | virtio GPU, 800 x 480, seen by the program as `/dev/fb0` (32 bits per pixel) | A framebuffer device, as on a Pi, but not the Pi's GPU or panel. |
| Touch | virtio tablet (an absolute pointer), seen as `/dev/input/event0` | An evdev touch device, as on a Pi, but not a real touch controller. |
| Network and PLC | QEMU user network; the fake PLC runs on the host and the program connects to it at 10.0.2.2 | A real Modbus TCP connection through the guest's network stack, to a simulated PLC, not a real one. |

## What it proves

- The aarch64 static binary starts, verifies its signed configuration, draws the interface through `/dev/fb0` and reads touch events from an evdev device, on a 64-bit ARM Linux kernel.
- The program's own 32-bit framebuffer path works (it renders RGB565 and converts each strip to 32 bits per pixel).
- Modbus TCP polling, alarms, acknowledgement, an authorised setpoint write with read-back confirmation, loss of the PLC link and its recovery all work in that environment.
- The memory the program uses as a process on 64-bit ARM Linux (measured inside the guest with `/proc/<pid>/smaps_rollup`).

## What it does not prove

- Anything about the speed of a real Pi. The CPU is emulated, so start-up time, rendering time, touch latency and CPU use measured here are not Pi figures and are not reported as such.
- The real Pi display driver and panel, real touch calibration, readability of the font on a real panel, a 16 bits per pixel framebuffer (the emulated GPU offers only 32 bits; the 16-bit code path has not run on any framebuffer), real systemd and the root helper under systemd, power loss, SD card behaviour, a real PLC and a real network.

## How to run it (Windows 11 with WSL2, Ubuntu 24.04; or Linux)

Prerequisites: `qemu-system-arm`, `qemu-system-gui`, `libsdl2-dev` (only for the host build of the fake PLC), `curl`, `squashfs-tools`, `cpio`, `python3`, `kmod`, and the aarch64 musl compiler from https://musl.cc/aarch64-linux-musl-cross.tgz unpacked to `/opt/toolchains/aarch64-linux-musl-cross` (any aarch64 musl cross compiler works; change `CC`).

```sh
# 1. the program for the target and the fake PLC for the host
make -f Makefile.linux STATIC=1 CC=/opt/toolchains/aarch64-linux-musl-cross/bin/aarch64-linux-musl-gcc BUILD=build-aarch64 hmi
make -f Makefile.linux BUILD=build-sdl build-sdl/fake_plc
# 2. fetch the kernel and modules (about 90 MB, once)
bash emulation/pi/prepare.sh
# 3. pack the guest
bash emulation/pi/make_initramfs.sh
# 4a. a visible emulated screen (a window; WSLg on Windows 11). Click it like a touch screen.
bash emulation/pi/run_qemu.sh window
# 4b. no window; a scripted tour that taps the emulated touch screen and saves screenshots of the emulated display
bash emulation/pi/run_qemu.sh headless &
python3 emulation/pi/drive.py /root/work/pi/run evidence/pi_emulated
```

The guest prints its console (kernel messages, the program's messages, and its memory every 5 seconds) to `guest.log` in the run directory.

## Differences from a real Raspberry Pi deployment

On a Pi the program reads `/dev/fb0` of the Pi's graphics driver, uses `/dev/input/eventN` of the touch panel, and runs under systemd with the units in `deploy/`. The program and its command line are the same. `ROLE` and the configuration are given as in the README.
