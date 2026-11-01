# Screens of the HMI

This file describes the screens of the Linux HMI as they exist after milestone M4 and the size work of 2026-10-23 (no login, one font, a 16-colour palette). Everything
here was checked in the headless UI harness (`tests/hmi/ui_headless.c`, real LVGL 9.2.2, a
memory display, no touch) and in the view-model tests on the Windows host and on WSL2. None of
it was run on a Raspberry Pi, on a real framebuffer or with real touch input. The screenshots
the harness writes (`build/shot_*.ppm`) are host renderings, not photographs of a device.

## Layout

The display is 800 by 480. A header (the PLC link, the alarm summary, the claimed role, ACK ALL) is
on top. There is no login: the role is the one the program was started with (`--role`, default
viewer) and is shown in capitals; above viewer it is drawn in the warning colour, and the
SYSTEM view and the bottom of the overview say that it is not authenticated
(`docs/AUTHENTICATION.md`). A tab bar below the header has four tabs: OVERVIEW, ALARMS, TRENDS
and SETTINGS (three in the lite profile, which has no TRENDS). Each screen has its own page of
800 by 400 pixels, starting at y = 80. Only the visible page is updated by the binder. The
overview and the alarm list are built at start; TRENDS and the CONFIG view are built the first
time they are opened (`lazy` in `ui_screen_ops_t`) and then stay. A screen is added by writing
its file and adding its table to `SCREENS` in `src/ui/ui_nav.c`; the views of the settings
screen work the same way (`VIEWS` in `src/ui/ui_settings.c`).

All wording and tone decisions are in `src/ui/hmi_view.c` (pure functions, unit tested). The
files in `src/ui` only map a tone to a colour and set label text. Every action a button runs
goes through the core, which decides again (`hmi_authz`, `hmi_writes`, `hmi_cfgedit`); a
hidden or disabled button is a convenience, not the protection.

## Colours, font and drawing

- **Palette.** The whole interface uses the eleven colours defined as `C_*` in `src/ui/ui_priv.h`
  (background, panel, line, text, muted, dim, good, warning, bad, accent, second chart line).
  That is inside a 16-colour palette. Styles are in one place, `src/ui/ui_theme.c`; LVGL's
  default theme, its shadows, gradients, rounded corners and press animations are not compiled
  in. Corners are square, fills are solid and borders are one pixel.
- **Font.** One font, DejaVu Sans 14 px, one bit per pixel, printable ASCII only
  (`src/ui/font/README.md`). One bit per pixel means no blended edge colours.
- **Colour depth.** LVGL renders into RGB565 and only RGB565 is compiled into its drawing code
  (`LV_DRAW_SW_SUPPORT_*`). A framebuffer of another depth is refused at start. LVGL 9.2.2 has
  no 4-bit-per-pixel target for its software renderer: `LV_COLOR_FORMAT_I4` exists for images
  but not as a display format, so a true 4-bit framebuffer needs a conversion step in the flush
  callback (RGB565 to a 16-entry palette index, two pixels per byte). The screenshot tool
  (`tests/hmi/shot_tool.py i4`) does exactly that conversion on the screenshots and shows that
  it is exact for every screen except the chart (see "Screenshot tests"): a 4-bit panel would
  need 192 KB of framebuffer instead of 750 KB. No 4-bit display driver was written, because the
  Raspberry Pi framebuffer is 16 or 32 bits.
- **Draw buffer.** The fbdev driver draws through a partial buffer of 20 lines (32 KB), not the
  library's 60 (96 KB).

## Screenshot tests

`tests/hmi/ui_headless.c` writes a PPM of each state to `build-linux/shot_*.ppm`.
`tests/hmi/shot_tool.py` converts them to PNG, counts the colours and checks them against the
palette. The PNG copies of the latest run are in `bench/results/screens/`. See
`docs/OPTIMIZATION_ROADMAP.md`, section 8, for the list of states and the findings.

## The screens

| Screen | File | What it shows and does | Who |
|---|---|---|---|
| OVERVIEW | `ui_overview.c` | Tag table, ten rows per page with page buttons; value, quality and alarm state per tag. A setpoint panel with SEND (a write goes through `hmi_writes`). A line with the latest journal entries. A banner when storage has a fault. | Everyone sees it. Writes need the role of the tag. |
| ALARMS | `ui_alarms.c` | The alarm list, most urgent first (priority, then unacknowledged, then newest), eight rows per page. A per-alarm ACK button that refuses an alarm that changed after it was drawn. | Everyone sees it. ACK needs Operator or above. |
| TRENDS | `ui_trends.c` | A min and max chart of one trended tag over a window of 10, 20 or 40 minutes with the default 5 s bucket. Only GOOD data is kept; gaps are shown as gaps. Data is in RAM only and is lost at restart. | Everyone |
| SETTINGS | `ui_settings.c` | A list of views on the left and one view on the right. | See below. |

### Settings views

| View | File | What it does | Who |
|---|---|---|---|
| SYSTEM | `ui_settings.c` | Seven read-only lines: PLC address, writes on or off and why, the configuration in use (signed, serial), storage state, clock state, tag counts, build kind. Refreshed at most once a second. | Everyone |
| CONFIG | `ui_config.c` | Edits to alarm settings and the PLC address, sent as one request. See below. | Engineer and above, and only when the service was started with `--spool` and a verified configuration |

### The CONFIG view

The selector at the top steps through the PLC connection and every tag. For the PLC it shows
HOST, PORT and MODBUS UNIT. For a tag it shows HIGH-HIGH, HIGH, LOW, LOW-LOW, DEADBAND, ON DELAY,
OFF DELAY and PRIORITY. Each row shows the current value and, after an edit, `-> new value`.

- Pressing PRIORITY steps through low, medium, high and critical. Pressing HOST steps through
  the hosts of `plc.allowed_hosts` only. Every other row opens a number pad (digits, one point,
  a minus sign first only, DEL, OK, CANCEL). OFF is offered for the four limits only.
- An edit that equals the value in the running configuration is removed again, so an edit is
  always a real difference.
- APPLY needs two presses within 5 seconds. The first press runs the same rules that the root
  helper will run (`hmi_cfgedit_apply` on the running configuration) and shows the first
  refusal at once, for example "Refused: an alarm limit must lie inside the tag's min and max".
  Any new edit, DISCARD, or a lapse of 5 seconds cancels the question. The second press hands
  one request to the spool directory (`hmi_cfgreq_submit`): the core checks the role, the audit
  line `CONFIG_REQUEST by=- changes=<n> base_serial=<s>` is made durable first, and only
  then is `<spool>/request` written.
- The view only sends a request. The configuration is changed by `p99_cfg` (root, started by
  `deploy/prometheus99-config.path`), which re-checks everything, writes the audit line
  `CONFIG_CHANGED`, signs the new configuration with serial plus one and restarts the service.
  The line "Last request" shows what the helper answered. See `docs/CONFIG_INTEGRITY.md`.
- A viewer or an operator sees "Changing settings needs an engineer or an administrator". A
  device without `--spool` says that changing settings is not set up. When a session ends,
  edits that were not sent are dropped.

What a request can change is the whitelist in `src/hmi/hmi_cfgedit.h`: the PLC host (only a
host that is already in `plc.allowed_hosts`), the port and the unit, and per tag the four alarm
limits (each can be switched off), the deadband, the two delays and the priority. It cannot
change the write switch, who may write, write limits, rate limits, timeouts, the allow-list,
tag addresses, types or scaling, and it cannot add or remove tags. Those need the offline
tools and a new signature.

## Test seams

`src/ui/hmi_ui.h` lists one function per control, for example `hmi_ui_press_config_row`,
`hmi_ui_press_config_key` and `hmi_ui_press_config`. Each runs exactly what the on-screen
control runs, including for roles whose button is disabled, so the tests can show that the core
refuses. The headless scenario uses them (277 assertions on both hosts).

## Memory

On the host the LVGL pool is `LV_MEM_SIZE` 112 KB (`config/lv_conf.h`). With the screens that are
built at start the harness reports 48 KB in use, and 74 KB as the peak over the whole scenario,
which opens every screen and the CONFIG number pad; there is no heap allocation in two seconds
of steady state. In the lite profile (no TRENDS, no CONFIG) the figures are 46 KB and 47 KB. The
harness fails when the pool after the build exceeds 56 KB or the peak exceeds 88 KB (56 KB lite),
so the screens cannot grow back unnoticed. Before this work the same harness reported 119 KB in
use of 172 KB (peak 122 KB). These are host numbers; the target may differ.

## Not verified

- Rendering and touch on a real display. The number pad key size (96 by 46 pixels) was not tried
  with a finger.
- A 14 px font of one bit per pixel on a real panel at its real size; readability was judged on
  screenshots only.
- That the restart after an applied change behaves under real systemd.
- Long-term behaviour of the 20 to 30 minute trend windows on the target.
