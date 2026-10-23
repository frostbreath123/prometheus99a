# Prometheus99 build (Windows host, MinGW-w64 GCC + MSYS sh/find/ar).
#
# Targets
#   all          legacy Windows prototype (prometheus99.exe, custom renderer)
#   test         legacy prototype unit tests (layers 1-2)
#   test-hmi     portable HMI runtime host tests (mock PLC, fake clock)
#   test-e2e     runtime + real libmodbus against an in-process fake PLC
#   ui-host      real LVGL v9.2.2 headless UI harness + scenario run
#   test-all     test + test-hmi + test-e2e
#   check-linux  compile-check src/app/main_linux.c, production and HMI_TEST_ROLE variants (no link, no run)
# The production Linux target is built with Makefile.linux on Linux.

# Presentation layer: one file per screen plus the shared header (see src/ui/ui_priv.h).
UI_SRC := src/ui/hmi_ui.c src/ui/ui_ack.c src/ui/ui_nav.c src/ui/ui_overview.c src/ui/ui_alarms.c src/ui/ui_trends.c src/ui/ui_settings.c src/ui/ui_config.c src/ui/ui_theme.c src/ui/font/p99_font.c
CC ?= gcc
BUILD = build

# ---------------- legacy prototype ----------------
CFLAGS = -std=c99 -Wall -Wextra -Wpedantic -Os -DNDEBUG -Iinclude \
         -ffunction-sections -fdata-sections -fstack-protector-strong
LDFLAGS = -lgdi32 -luser32 -lwinmm -Wl,--gc-sections -Wl,--stack,65536 \
          -Wl,--dynamicbase -Wl,--nxcompat -s
TARGET = prometheus99.exe
SRCS = src/layer0_hardware.c src/layer1_hal.c src/layer2_core.c src/layer3_presentation.c src/main.c
TEST_SRCS = tests/test_core.c tests/stub_layer0.c src/layer1_hal.c src/layer2_core.c
TEST_TARGET = tests/test_core.exe

all: $(TARGET)

$(TARGET): $(SRCS) $(wildcard include/*.h)
	$(CC) $(CFLAGS) $(SRCS) -o $(TARGET) $(LDFLAGS)

test: $(TEST_TARGET)
	./$(TEST_TARGET)

$(TEST_TARGET): $(TEST_SRCS) $(wildcard include/*.h tests/*.h)
	$(CC) -std=c99 -Wall -Wextra -Wpedantic -O1 -g -Iinclude $(TEST_SRCS) -o $(TEST_TARGET)

# ---------------- portable HMI runtime ----------------
HMI_CORE = src/hmi/hmi_decode.c src/hmi/hmi_tags.c src/hmi/hmi_comms.c src/hmi/hmi_writes.c src/hmi/hmi_alarm.c \
           src/hmi/hmi_config.c src/hmi/hmi_runtime.c src/hmi/hmi_watchdog.c src/hmi/hmi_authz.c src/hmi/hmi_cli.c src/hmi/hmi_util.c src/hmi/hmi_mac.c src/hmi/hmi_cfgsec.c src/hmi/hmi_store.c src/hmi/hmi_auditfmt.c src/hmi/hmi_trend.c src/hmi/hmi_config_write.c src/hmi/hmi_cfgedit.c third_party/monocypher/monocypher.c third_party/monocypher/monocypher-ed25519.c
HMI_PLAT = src/hmi/hmi_platform_win32.c
HMI_HDRS = $(wildcard src/hmi/*.h src/ui/*.h tests/hmi/*.h)
HMI_WARN = -Wall -Wextra -Wpedantic

LIBMODBUS_INC = -DHAVE_CONFIG_H -Ithird_party/libmodbus_cfg -Ithird_party/libmodbus/src
LIBMODBUS_OBJ = $(BUILD)/modbus.o $(BUILD)/modbus-tcp.o $(BUILD)/modbus-data.o
LVGL_INC = -DLV_CONF_INCLUDE_SIMPLE -Iconfig -Ithird_party/lvgl

$(BUILD):
	mkdir -p $(BUILD)

$(LIBMODBUS_OBJ): $(BUILD)/%.o: third_party/libmodbus/src/%.c | $(BUILD)
	$(CC) -std=gnu99 -O2 $(LIBMODBUS_INC) -c $< -o $@

$(BUILD)/liblvgl.a: config/lv_conf.h | $(BUILD)
	rm -rf $(BUILD)/lvgl && mkdir -p $(BUILD)/lvgl
	find third_party/lvgl/src -name '*.c' | xargs -P 8 -I{} sh -c \
	  'o=$(BUILD)/lvgl/$$(echo {} | sed "s|third_party/lvgl/src/||; s|/|__|g; s|\.c$$|.o|"); \
	   $(CC) -std=gnu99 -Os -ffunction-sections -fdata-sections $(LVGL_INC) -c {} -o $$o'
	ar rcs $@ $(BUILD)/lvgl/*.o

HMI_TESTS = decode tags comms link_health writes alarm config runtime watchdog authz cli platform mac cfgsec store auditfmt trend cfgedit
HMI_TEST_EXES = $(patsubst %,$(BUILD)/hmi_tests/test_%.exe,$(HMI_TESTS))

# One program per module; each links the shared fixtures in test_support.c.
$(BUILD)/hmi_tests/test_%.exe: tests/hmi/test_%.c tests/hmi/test_support.c tests/hmi/test_support.h \
                               $(HMI_CORE) $(HMI_PLAT) $(HMI_HDRS) | $(BUILD)
	mkdir -p $(BUILD)/hmi_tests
	$(CC) -std=c99 $(HMI_WARN) -O1 -g -Isrc/hmi $< tests/hmi/test_support.c $(HMI_CORE) $(HMI_PLAT) -o $@ -lm

$(BUILD)/test_e2e.exe: tests/hmi/test_modbus_e2e.c tests/hmi/fake_modbus_server.c src/hmi/plc_libmodbus.c \
                       $(HMI_CORE) $(HMI_PLAT) $(HMI_HDRS) $(LIBMODBUS_OBJ) | $(BUILD)
	$(CC) -std=gnu99 -Wall -Wextra -O1 -g $(LIBMODBUS_INC) -Isrc/hmi tests/hmi/test_modbus_e2e.c \
	  tests/hmi/fake_modbus_server.c src/hmi/plc_libmodbus.c $(HMI_CORE) $(HMI_PLAT) $(LIBMODBUS_OBJ) \
	  -o $@ -lws2_32 -lm

$(BUILD)/ui_headless.exe: tests/hmi/ui_headless.c tests/hmi/fake_modbus_server.c tests/hmi/alloc_count.c \
                          $(UI_SRC) src/ui/hmi_view.c src/hmi/plc_libmodbus.c $(HMI_CORE) $(HMI_PLAT) $(HMI_HDRS) \
                          $(LIBMODBUS_OBJ) $(BUILD)/liblvgl.a | $(BUILD)
	$(CC) -std=gnu99 -Wall -Wextra -Os $(LVGL_INC) $(LIBMODBUS_INC) tests/hmi/ui_headless.c \
	  tests/hmi/fake_modbus_server.c tests/hmi/alloc_count.c $(UI_SRC) src/ui/hmi_view.c src/hmi/plc_libmodbus.c \
	  $(HMI_CORE) $(HMI_PLAT) $(LIBMODBUS_OBJ) $(BUILD)/liblvgl.a -o $@ -lws2_32 -lpsapi -lm \
	  -Wl,--wrap=malloc -Wl,--wrap=calloc -Wl,--wrap=realloc -Wl,--wrap=free

$(BUILD)/test_view.exe: tests/hmi/test_hmi_view.c src/ui/hmi_view.c $(HMI_CORE) $(HMI_PLAT) $(HMI_HDRS) | $(BUILD)
	$(CC) -std=c99 $(HMI_WARN) -O1 -g -Isrc/hmi tests/hmi/test_hmi_view.c src/ui/hmi_view.c $(HMI_CORE) $(HMI_PLAT) -o $@ -lm

$(BUILD)/config_golden.exe: tests/hmi/config_golden.c src/hmi/hmi_config.c src/hmi/hmi_config_write.c src/hmi/hmi_tags.c src/hmi/hmi_decode.c \
                            $(HMI_PLAT) $(HMI_HDRS) | $(BUILD)
	$(CC) -std=c99 $(HMI_WARN) -O1 -g -Isrc/hmi tests/hmi/config_golden.c src/hmi/hmi_config.c src/hmi/hmi_config_write.c \
	  src/hmi/hmi_tags.c src/hmi/hmi_decode.c $(HMI_PLAT) -o $@ -lm

# Characterization: every accept/reject verdict, error text and parsed field
# must match the golden file generated by the pre-table parser.
test-config-golden: $(BUILD)/config_golden.exe
	./$(BUILD)/config_golden.exe > $(BUILD)/config_golden.out
	diff -u tests/hmi/config_golden.txt $(BUILD)/config_golden.out && echo "config golden: identical"

# Live demo window (Win32, no extra library): the real UI and runtime against the fake PLC, with a
# stand-in for the root helper. A demonstration build, not the product (see demo/hmi_win_demo.c).
$(BUILD)/hmi_win_demo.exe: demo/hmi_win_demo.c tests/hmi/fake_modbus_server.c \
                           $(UI_SRC) src/ui/hmi_view.c src/hmi/plc_libmodbus.c $(HMI_CORE) $(HMI_PLAT) $(HMI_HDRS) \
                           $(LIBMODBUS_OBJ) $(BUILD)/liblvgl.a | $(BUILD)
	$(CC) -std=gnu99 -Wall -Wextra -Os $(LVGL_INC) $(LIBMODBUS_INC) -Isrc/hmi demo/hmi_win_demo.c \
	  tests/hmi/fake_modbus_server.c $(UI_SRC) src/ui/hmi_view.c src/hmi/plc_libmodbus.c \
	  $(HMI_CORE) $(HMI_PLAT) $(LIBMODBUS_OBJ) $(BUILD)/liblvgl.a -o $@ -mwindows -lws2_32 -lgdi32 -luser32 -lm

demo-live: $(BUILD)/hmi_win_demo.exe
	cd $(BUILD) && ./hmi_win_demo.exe

test-hmi: $(HMI_TEST_EXES)
	sh tests/hmi/run_core_tests.sh $(BUILD)/hmi_tests $(HMI_TESTS)

test-view: $(BUILD)/test_view.exe
	./$(BUILD)/test_view.exe

test-e2e: $(BUILD)/test_e2e.exe
	./$(BUILD)/test_e2e.exe

ui-host: $(BUILD)/ui_headless.exe
	./$(BUILD)/ui_headless.exe $(BUILD)/shot

test-all: check-tagdb-access test test-hmi test-view test-config-golden test-e2e

check-linux:
	$(CC) -std=gnu99 -fsyntax-only -Wall -Wextra -DP99_LINUX_TARGET $(LVGL_INC) \
	  -Ithird_party/libmodbus_cfg -Ithird_party/libmodbus/src src/app/main_linux.c
	$(CC) -std=gnu99 -fsyntax-only -Wall -Wextra -DP99_LINUX_TARGET -DHMI_TEST_ROLE=1 $(LVGL_INC) \
	  -Ithird_party/libmodbus_cfg -Ithird_party/libmodbus/src src/app/main_linux.c

clean:
	rm -f $(TARGET) $(TEST_TARGET) src/*.o
	rm -rf $(BUILD)

.PHONY: all test test-hmi demo-live test-view test-config-golden test-e2e ui-host test-all check-linux clean

# Outside hmi_tags.c (and the white-box core test) nothing may read or write
# the tag table's members directly; use the hmi_tagdb_* accessors.
TAGDB_GUARD_FILES = $(filter-out src/hmi/hmi_tags.c,$(wildcard src/hmi/*.c src/hmi/*.h src/ui/*.c src/ui/*.h src/app/*.c)) \
                    tests/hmi/ui_headless.c tests/hmi/test_modbus_e2e.c tests/hmi/test_hmi_view.c
check-tagdb-access:
	@if grep -nE '\bdb(\.|->)(cfg|val|groups|order|n_tags|n_groups|global_version|lock|ready)\b' $(TAGDB_GUARD_FILES); then \
	  echo "check-tagdb-access: FAIL (use hmi_tagdb_* accessors)"; exit 1; \
	else echo "check-tagdb-access: ok"; fi

.PHONY: check-tagdb-access
