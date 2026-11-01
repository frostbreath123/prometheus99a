# The source lists of the portable HMI program, shared by Makefile (Windows), Makefile.windows and Makefile.linux.
# Add or remove a source file here, once.

# Presentation layer: one file per screen plus the shared header (see src/ui/ui_priv.h).
UI_SRC := src/ui/hmi_ui.c src/ui/ui_ack.c src/ui/ui_nav.c src/ui/ui_overview.c src/ui/ui_alarms.c src/ui/ui_trends.c src/ui/ui_settings.c src/ui/ui_config.c src/ui/ui_theme.c src/ui/font/p99_font.c

# The portable core, without the operating-system file (hmi_platform_posix.c on Linux, hmi_platform_win32.c on Windows).
HMI_CORE_COMMON = src/hmi/hmi_decode.c src/hmi/hmi_tags.c src/hmi/hmi_comms.c src/hmi/hmi_writes.c src/hmi/hmi_alarm.c \
           src/hmi/hmi_config.c src/hmi/hmi_runtime.c src/hmi/hmi_watchdog.c src/hmi/hmi_authz.c src/hmi/hmi_cli.c src/hmi/hmi_util.c src/hmi/hmi_mac.c src/hmi/hmi_cfgsec.c src/hmi/hmi_store.c src/hmi/hmi_auditfmt.c src/hmi/hmi_trend.c src/hmi/hmi_config_write.c src/hmi/hmi_cfgedit.c src/hmi/hmi_boot.c third_party/monocypher/monocypher.c third_party/monocypher/monocypher-ed25519.c
