# Prometheus99 Lightweight HMI Runtime Makefile
# C99 Standard + Static Memory Architecture

CC ?= gcc

# -Os: Optimize for size (smaller binary than -O2)
# -ffunction-sections -fdata-sections: Place each function/data in its own section
# -DNDEBUG: Disable assert overhead in release builds
CFLAGS = -std=c99 -Wall -Wextra -Wpedantic -Os -DNDEBUG -Iinclude \
         -ffunction-sections -fdata-sections

# -Wl,--gc-sections: Strip unreferenced sections at link time (dead code elimination)
# -s: Strip debug symbols from binary
# NOTE: No -static flag — dynamic linking against system DLLs keeps binary small
LDFLAGS = -lgdi32 -luser32 -lwinmm -lpsapi -Wl,--gc-sections -Wl,--stack,65536 -s

TARGET = prometheus99.exe
SRCS = src/layer0_hardware.c \
       src/layer1_hal.c \
       src/layer2_core.c \
       src/layer3_presentation.c \
       src/main.c

OBJS = $(SRCS:.c=.o)

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) $(SRCS) -o $(TARGET) $(LDFLAGS)

clean:
	rm -f $(TARGET) src/*.o

.PHONY: all clean
