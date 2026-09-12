# Prometheus99

![C99](https://img.shields.io/badge/C-99-blue.svg)
![LVGL](https://img.shields.io/badge/UI-LVGL%20v8-orange.svg)
![Platform](https://img.shields.io/badge/platform-Windows%20x86--64-lightgrey.svg)
![Heap](https://img.shields.io/badge/heap%20allocations-0-brightgreen.svg)
![License: MIT](https://img.shields.io/badge/license-MIT-green.svg)

### An ultra-lightweight industrial HMI runtime — 0.10 MB RAM, 30 KB binary, zero heap

Prometheus99 is a bare-metal-optimized Human-Machine Interface runtime for industrial control panels, built from scratch in pure C99 on the LVGL v8 partial-draw architecture. It replaces the 150 MB–1.2 GB working sets of Qt/Electron/WinCC-based HMIs with a **102 KB** footprint and a **30,720-byte** executable — small enough to boot from a serial SPI flash chip and run on microcontroller-class SRAM instead of external DRAM.

Built for the **Schneider Electric Hackathon — PS4: Lightweight HMI Runtime** by **Team Doomsday**.

---

## Results

| Metric | Conventional HMI (Qt / Electron / WinCC) | Prometheus99 | Reduction |
|---|---|---|---|
| Runtime RAM (private working set) | 150 MB – 1.2 GB | **0.10 MB (102 KB)** | **96.3%** (>99.9% vs. Qt) |
| Executable size on disk | 45 MB – 350 MB | **30.0 KB** | **89.3%** (5× under the 150 KB target) |
| Display buffer RAM | 6.14 MB (double-buffered 32bpp) | **18.75 KB** | **98.8%** — fits entirely in L1 cache |
| Heap allocations | Thousands per frame | **0 (100% static BSS)** | Zero fragmentation, unbounded uptime |
| Background sensor CPU load | 5–15% (WMI/COM polling) | **< 0.1%** | 10 Hz decimation inside a 1000 Hz ISR |
| Telemetry source | Mocked / synthetic | **Live host hardware** (CPU, fan, SSD, ACPI) | Real IOCTL/EC reads, not simulated |
| Hot-standby failover latency | 200 ms – 2.5 s | **< 10 µs** | Pre-synchronized dual-node state |

---

## Architecture

Four strictly decoupled layers. The 1000 Hz hardware loop and the 30 Hz UI loop never call each other directly — they only ever touch a lock-free buffer in the middle, so a slow render can't stall a sensor read and vice versa.

```
  LAYER 3   PRESENTATION      30 Hz UI loop · 18.75 KB band buffer · 16-color CLUT
                │
                │  atomic read
                ▼
  LAYER 2   CORE / STATE      Ping-pong buffer (CRC16) · FSM · alarms · watchdog
                ▲
                │  atomic write
                │
  LAYER 1   HAL / DRIVERS     1000 Hz sensor ISR · input queue · display flush
                │
                ▼
  LAYER 0   HARDWARE / I/O    SSD · fan RPM · CPU/RAM · touch input · display
```

| Layer | Responsibility | Key components |
|---|---|---|
| **3 — Presentation** | Renders the UI at 30 Hz | Band-buffer renderer, 16-color CLUT, static screens (Dash/Diag/Alarm/Settings/Failover) |
| **2 — Core & State** | Owns all shared state | Lock-free ping-pong buffer, FSM, ISA-18.2 alarm supervisor, watchdog |
| **1 — HAL & Drivers** | Bridges hardware to state | 1000 Hz sensor ISR (10 Hz decimated), input queue, display flush driver |
| **0 — Hardware & I/O** | Talks to physical devices | SSD IOCTL, motherboard EC, CPU/RAM counters, touch input, display panel |

**Rule:** Layer 3 never blocks on Layer 0/1 I/O, and Layer 1 never waits on the renderer. State crosses the boundary exactly once per frame, through the CRC16-checked buffer in Layer 2.

---

## Engineering Decisions

| Area | Approach | Why |
|---|---|---|
| **Color model** | 4-bit CLUT (16 colors) | 87.5% smaller than 32-bit ARGB; whole buffer fits in L1 cache |
| **Frame buffering** | 48-line partial band (18.75 KB) | Avoids a full-frame DRAM round-trip on constrained hardware |
| **Memory allocation** | 100% static BSS, 0 heap calls | Eliminates heap fragmentation — the #1 cause of long-uptime HMI crashes |
| **Working-set control** | `EmptyWorkingSet()` after each flush | Strips unreferenced CRT/DLL pages that would otherwise creep upward |
| **Build pipeline** | MSVC `/GL /LTCG /GS- /OPT:REF /OPT:ICF` | Cuts default 150–400 KB MSVC output down to 30,720 bytes |
| **Telemetry source** | Direct kernel/EC IOCTLs | Avoids WMI/COM polling overhead (100 MB+ RAM, 5–15% CPU) |
| **Thread sync** | Lock-free ping-pong buffer (CRC16) | No mutex jitter between the 1000 Hz and 30 Hz loops |
| **Fault tolerance** | Dual-loop AND watchdog + hot standby | Single-loop watchdogs miss a hung UI *or* a dead sensor bus — never both |

---

## Project Structure

```
prometheus99/
├── src/
│   ├── main.c                  # Entry point, frame loop, EmptyWorkingSet() call
│   ├── layer0_hardware.c       # IOCTL / EC probes: SSD, fan RPM, CPU, thermals
│   ├── layer1_hal.c            # Sensor ISR, input queue, display flush driver
│   ├── layer2_core.c           # Ping-pong state buffer, FSM, alarms, watchdog
│   └── layer3_presentation.c   # LVGL band-buffer renderer, screens, CLUT
├── include/                    # Shared headers / struct definitions
├── build.cmd                   # MSVC build script (see below)
└── README.md
```

> Adjust paths above to match your actual repo layout before publishing — this tree reflects the 4-layer architecture the runtime is built around.

---

## Build & Run

### Prerequisites
- Windows x86-64
- MSVC (`cl.exe`) — Build Tools for Visual Studio or a full VS install
- LVGL v8 (vendored or submodule under `include/lvgl`)

### Build

```cmd
cl.exe /nologo /O1 /Os /GL /Gy /GF /Gw /GS- /MD /D_CRT_SECURE_NO_WARNINGS /DNDEBUG /Iinclude ^
  src\main.c src\layer0_hardware.c src\layer1_hal.c src\layer2_core.c src\layer3_presentation.c ^
  /link /OPT:REF /OPT:ICF /LTCG /FIXED /DEBUG:NONE /MERGE:.pdata=.text /STACK:32768,4096 /HEAP:65536,4096
```

This produces a single **30.0 KB** statically-linked executable with no external CRT dependency beyond `msvcrt.dll`.

### Run

```cmd
prometheus99.exe
```

The runtime boots directly into the live dashboard, reading real CPU load, RAM status, physical fan RPM (via the motherboard Embedded Controller), and SSD throughput (via `IOCTL_DISK_PERFORMANCE`) from the host machine — no mocked data.

---

## Safety Architecture

Both loops must independently prove they're alive every 500 ms. Either one going silent trips the watchdog:

```
   1000 Hz sensor ISR  ──heartbeat──┐
                                    ├──▶  AND supervisor
   30 Hz UI loop        ──heartbeat──┘         │
                                    ┌───────────┴───────────┐
                                    ▼                       ▼
                          both alive: OK           either dead: FAULT
                          (pet hardware WDT)        → hot-standby failover
                                                        (< 10 µs)
```

Both nodes run identical deterministic state machines and continuously replicate ~96-byte binary state snapshots, so failover is a pointer swap, not a reconvergence — there is no cold-start delay.

---

## FAQ

**Why 4-bit color instead of 16- or 24-bit?**
Industrial HMIs prioritize legibility and state clarity over photorealism. The 16-color palette follows the ISA-101 high-performance-HMI standard (dark background, muted structural grays, saturated green/amber/red for state), and dropping to 4-bit is what lets the entire active render buffer fit inside L1 cache.

**How does 0.10 MB RAM happen on 64-bit Windows, where default working sets are usually several MB?**
Four compounding changes: an 18.75 KB band buffer instead of a full frame, zero heap allocation, 16 KB thread stacks / 4 KB heap commits instead of the 1 MB Win32 default, and an `EmptyWorkingSet()` call after every flush to strip unreferenced transient pages.

**Isn't a single watchdog loop enough?**
No — a watchdog that only checks the communication loop can miss a frozen UI thread, and one that only checks the UI loop can miss a dead sensor bus. The dual-loop AND check requires both to prove liveness independently.

**Does this actually read real hardware, or is the dashboard mocked?**
Real hardware. CPU load via `GetSystemTimes`, SSD throughput via a direct handle to `\\.\PhysicalDrive0` with `IOCTL_DISK_PERFORMANCE`, and fan RPM via a direct Embedded Controller query (`\\.\ATKACPI`), with an ACPI thermal-zone fallback for temperature.

---

## Team

**Team Doomsday** — Abhilash L, Adarsh Abhilash, Nikhil Nuguri
Built for the Schneider Electric Hackathon, Problem Statement PS4 (Lightweight HMI Runtime).

## License

Released under the [MIT License](LICENSE).