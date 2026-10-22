# Screens of the HMI

This file describes the screens of the Linux HMI as they exist after milestone M4. Everything
here was checked in the headless UI harness (`tests/hmi/ui_headless.c`, real LVGL 9.2.2, a
memory display, no touch) and in the view-model tests on the Windows host and on WSL2. None of
it was run on a Raspberry Pi, on a real framebuffer or with real touch input. The screenshots
the harness writes (`build/shot_*.ppm`) are host renderings, not photographs of a device.

## Layout

The display is 800 by 480. A header (the PLC link, the alarm summary, the role and name of the
logged-in user, ACK ALL, LOGIN or LOGOUT) is on top. A tab bar below it has four tabs:
OVERVIEW, ALARMS, TRENDS and SETTINGS. Each screen is built once into its own page of 800 by
400 pixels, starting at y = 80. Only the visible page is updated by the binder. A screen is
added by writing its file and adding its table to `SCREENS` in `src/ui/ui_nav.c`; the views of
the settings screen work the same way (`VIEWS` in `src/ui/ui_settings.c`). The login and the
account dialogs are one layer drawn over everything (`src/ui/ui_login.c`).

All wording and tone decisions are in `src/ui/hmi_view.c` (pure functions, unit tested). The
files in `src/ui` only map a tone to a colour and set label text. Every action a button runs
goes through the core, which decides again (`hmi_authz`, `hmi_auth`, `hmi_writes`,
`hmi_cfgedit`); a hidden or disabled button is a convenience, not the protection.

## The screens

| Screen | File | What it shows and does | Who |
|---|---|---|---|
| OVERVIEW | `ui_overview.c` | Tag table, ten rows per page with page buttons; value, quality and alarm state per tag. A setpoint panel with SEND (a write goes through `hmi_writes`). A line with the latest journal entries. A banner when storage has a fault. | Everyone sees it. Writes need the role of the tag. |
| ALARMS | `ui_alarms.c` | The alarm list, most urgent first (priority, then unacknowledged, then newest), eight rows per page. A per-alarm ACK button that refuses an alarm that changed after it was drawn. | Everyone sees it. ACK needs Operator or above. |
| TRENDS | `ui_trends.c` | A min and max chart of one trended tag over a window of 10, 20 or 40 minutes with the default 5 s bucket. Only GOOD data is kept; gaps are shown as gaps. Data is in RAM only and is lost at restart. | Everyone |
| SETTINGS | `ui_settings.c` | A list of views on the left and one view on the right. | See below. |
| Login | `ui_login.c` | Login, first-boot creation of the administrator, and the dialogs for adding an account or setting a password. Passwords are never shown. | Everyone |

### Settings views

| View | File | What it does | Who |
|---|---|---|---|
| SYSTEM | `ui_settings.c` | Seven read-only lines: PLC address, writes on or off and why, the configuration in use (signed, serial), storage state, clock state, tag counts, build kind. Refreshed at most once a second. | Everyone |
| ACCOUNTS | `ui_users.c` | List of accounts (seven per page), ADD with the role of the next account, PASSWORD, ROLE (steps to the next role), REMOVE (two presses within 5 s). The core refuses your own account and the last administrator. Every change is recorded first, with `by=<administrator>`. | Administrator. Others see why the list is empty. |
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
  line `CONFIG_REQUEST by=<user> changes=<n> base_serial=<s>` is made durable first, and only
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

On the host the LVGL pool is `LV_MEM_SIZE` 192 KB (`config/lv_conf.h`). With every screen built
and the login layer, the harness reports 119 KB in use of 172 KB (peak 122 KB) and no heap
allocation in two seconds of steady state. The CONFIG view added about 20 KB because its
number pad and eight rows are built at start and hidden. These are host numbers; the target
may differ.

## Not verified

- Rendering and touch on a real display. The number pad key size (96 by 46 pixels) was not tried
  with a finger.
- That the restart after an applied change behaves under real systemd.
- Long-term behaviour of the 20 to 30 minute trend windows on the target.
