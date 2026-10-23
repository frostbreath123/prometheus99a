# bench: how Prometheus99 is measured

Everything here is a host measurement (x86-64, WSL2 Ubuntu 24.04). Nothing here has been run on a Raspberry Pi,
a real display or a real PLC, and the numbers must not be read as numbers for such a target.

## What the scripts do

| Script | What it measures | Output in `bench/results/linux/<label>/` |
|---|---|---|
| `measure_linux.sh <build dir> <label> [seconds]` | PSS, RSS, private memory, threads and CPU of the real executable, after the first frame and at the end of the run; every mapping from `smaps` | `summary.txt`, `samples.csv`, `smaps_first_frame.txt`, `smaps_end.txt`, `app.log` |
| `startup_linux.sh <build dir> <label> [runs]` | Time from process launch to a completely drawn first frame: one cold start (page cache dropped) and warm starts | `startup.txt` |
| `size_report.sh <executable> <label> [linker map]` | Stripped size, sections, the largest symbols and static objects, shared libraries, bytes per source group from the linker map | `size_report.txt` |
| `measure_process.ps1` | Windows only: working set of the legacy prototype (kept for history) | |

The executable that is measured is `prometheus99-bench` (`make -f Makefile.linux hmi-bench`). It is the product source with
one difference: a regular file stands in for `/dev/fb0` (`-DP99_FILE_FB` in `src/app/main_linux.c`), and there is no touch
input. The draw buffer has the size the fbdev driver would allocate (800 x 60 lines x 2 bytes), and the flush copies the rows
into the mapped file as the driver copies them into the device memory. The PLC is `build-linux/fake_plc`
(`tests/hmi/fake_plc_main.c`) on loopback. It answers instantly, so it shows the cost of the program and not of a network.

## The metric

`pss_kb` is the Pss line of `/proc/<pid>/smaps_rollup`. The mapping of the framebuffer file is reported on its own
(`pss_fb_mapping_kb`) and subtracted in `pss_excl_fb_kb`, because on a board the framebuffer belongs to the display driver.
Both figures are always printed.

## How to run

```bash
make -f Makefile.linux -j8 hmi-bench
bash bench/size_report.sh build-linux/prometheus99-bench my_label build-linux/prometheus99-bench.map
bash bench/startup_linux.sh build-linux my_label 15      # needs root for the cold start (drop_caches)
bash bench/measure_linux.sh build-linux my_label 600     # ten minutes
```

Keep other heavy work off the machine while a run is going; the CPU figures are tiny and easy to disturb.
