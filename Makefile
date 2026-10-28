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

# The portable HMI rules (test-hmi, test-view, test-config-golden, test-e2e, ui-host, demo-live) are in Makefile.windows,
# which is included below; the source lists are in sources.mk.

CC ?= gcc
BUILD = build
include sources.mk

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

include Makefile.windows

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
