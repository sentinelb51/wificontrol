# WiFi Control -- cross-built for Windows from any host with mingw-w64,
# with the policy core unit-tested natively.
#
#   make            build build/wificontrol.exe
#   make test       build and run the core tests on this host
#   make clean

CROSS   ?= x86_64-w64-mingw32-
WINCC    = $(CROSS)gcc
WINDRES  = $(CROSS)windres
CC      ?= cc
BUILD   ?= build

WARN     = -Wall -Wextra -Werror
WCFLAGS  = -std=c11 $(WARN) -Os -municode -D_WIN32_WINNT=0x0601 \
           -ffunction-sections -fdata-sections
WLDFLAGS = -municode -mwindows -Wl,--gc-sections -s
WLIBS    = -lwlanapi -lcomctl32 -lshell32 -lgdi32 -luser32

WSRC     = src/app.c src/wlan_core.c src/wlan_win32.c
WOBJ     = $(patsubst src/%.c,$(BUILD)/%.o,$(WSRC)) $(BUILD)/app.res.o

TESTBIN  = $(BUILD)/test_core
TESTFLAGS= -std=c11 $(WARN) -g -fsanitize=address,undefined

.PHONY: all test clean
all: $(BUILD)/wificontrol.exe

$(BUILD):
	@mkdir -p $(BUILD)

$(BUILD)/%.o: src/%.c | $(BUILD)
	$(WINCC) $(WCFLAGS) -c $< -o $@

$(BUILD)/app.res.o: src/app.rc src/resource.h src/app.manifest | $(BUILD)
	$(WINDRES) -I src $< -o $@

$(BUILD)/wificontrol.exe: $(WOBJ)
	$(WINCC) $(WOBJ) -o $@ $(WLDFLAGS) $(WLIBS)
	@echo "built $@ ($$(stat -c %s $@ 2>/dev/null || stat -f %z $@) bytes)"

test: $(TESTBIN)
	./$(TESTBIN)

$(TESTBIN): tests/test_core.c src/wlan_core.c src/wlan.h | $(BUILD)
	$(CC) $(TESTFLAGS) tests/test_core.c src/wlan_core.c -o $@

clean:
	rm -rf $(BUILD)

$(BUILD)/app.o:        src/wlan.h src/wlan_win32.h src/resource.h
$(BUILD)/wlan_core.o:  src/wlan.h
$(BUILD)/wlan_win32.o: src/wlan.h src/wlan_win32.h
