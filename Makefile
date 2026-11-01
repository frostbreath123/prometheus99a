# Prometheus99 build on a Windows host (MinGW-w64 GCC from MSYS2 UCRT64, with mingw32-make).
#
# This file only adds the repository-wide checks to the Windows rules in Makefile.windows (test-hmi, test-view,
# test-config-golden, test-e2e, ui-host, demo-live), which it includes; the source lists are in sources.mk.
# The device program is built with Makefile.linux on Linux.
#
# Targets of this file
#   test-all           check-tagdb-access + test-hmi + test-view + test-config-golden + test-e2e
#   check-linux        compile-check src/app/main_linux.c, production and HMI_TEST_ROLE variants (no link, no run)
#   check-tagdb-access nothing outside hmi_tags.c reads or writes the tag table's members directly
#   clean              remove the build directory

CC ?= gcc
BUILD = build
include sources.mk
include Makefile.windows

test-all: check-tagdb-access test-hmi test-view test-config-golden test-e2e

check-linux:
	$(CC) -std=gnu99 -fsyntax-only -Wall -Wextra -DP99_LINUX_TARGET $(LVGL_INC) \
	  -Ithird_party/libmodbus_cfg -Ithird_party/libmodbus/src src/app/main_linux.c
	$(CC) -std=gnu99 -fsyntax-only -Wall -Wextra -DP99_LINUX_TARGET -DHMI_TEST_ROLE=1 $(LVGL_INC) \
	  -Ithird_party/libmodbus_cfg -Ithird_party/libmodbus/src src/app/main_linux.c

clean:
	rm -rf $(BUILD)

# Outside hmi_tags.c (and the white-box core test) nothing may read or write
# the tag table's members directly; use the hmi_tagdb_* accessors.
TAGDB_GUARD_FILES = $(filter-out src/hmi/hmi_tags.c,$(wildcard src/hmi/*.c src/hmi/*.h src/ui/*.c src/ui/*.h src/app/*.c)) \
                    tests/hmi/ui_headless.c tests/hmi/test_modbus_e2e.c tests/hmi/test_hmi_view.c
check-tagdb-access:
	@if grep -nE '\bdb(\.|->)(cfg|val|groups|order|n_tags|n_groups|global_version|lock|ready)\b' $(TAGDB_GUARD_FILES); then \
	  echo "check-tagdb-access: FAIL (use hmi_tagdb_* accessors)"; exit 1; \
	else echo "check-tagdb-access: ok"; fi

.PHONY: test-all check-linux clean check-tagdb-access
