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

# Newest C standard each compiler accepts.  GCC only learned the -std=c23
# spelling in 14; 13 and earlier want -std=c2x for the same language.
newest_std = $(shell for s in c23 c2x c17 c11; do \
               $(1) -std=$$s -fsyntax-only -x c /dev/null >/dev/null 2>&1 && { echo $$s; break; }; done)
STD      ?= $(call newest_std,$(WINCC))
HOSTSTD  ?= $(call newest_std,$(CC))

WARN     = -Wall -Wextra -Werror

# Nothing that puts instructions on the hot path: no stack canary load and
# compare per frame, no endbr64 at every indirect branch target.
OPT     ?= -O3 -flto=auto -fuse-linker-plugin -fno-ident \
           -fno-stack-protector -fcf-protection=none

# Baseline is plain x86-64 so the binary runs anywhere.  ARCH=x86-64-v2
# (SSE4.2, ~2009+) or x86-64-v3 (AVX2, ~2013+) if you only target your own
# machines -- an older CPU faults on an unsupported instruction.
ARCH    ?=

WCFLAGS  = -std=$(STD) $(WARN) $(OPT) $(if $(ARCH),-march=$(ARCH)) \
           -municode -D_WIN32_WINNT=0x0601 -ffunction-sections -fdata-sections
# The PE mitigation bits below are header flags and load-time relocations.
# They execute nothing, so they cost no CPU and stay on.
WLDFLAGS = -municode -mwindows $(OPT) $(if $(ARCH),-march=$(ARCH)) \
           -Wl,--gc-sections -s \
           -Wl,--dynamicbase -Wl,--nxcompat -Wl,--high-entropy-va
WLIBS    = -lwlanapi -lcomctl32 -lshell32 -lgdi32 -luser32 \
           -lsetupapi -lcfgmgr32 -lpowrprof -ladvapi32 -luuid -luxtheme -ldwmapi \
           -liphlpapi

WSRC     = src/app.c src/app_ui.c src/ui_draw.c src/ui_ctl.c \
           src/wlan_core.c src/wlan_win32.c \
           src/tune.c src/tune_driver.c src/tune_power.c src/tune_ui.c \
           src/perf.c src/perf_win32.c
WOBJ     = $(patsubst src/%.c,$(BUILD)/%.o,$(WSRC)) $(BUILD)/app.res.o

# Some GCC builds (WinLibs, MSYS2) link a default-manifest.o into every exe.
# ld cannot merge it with ours, so both manifests end up in the binary.  An
# empty object of the same name, found first through -B, takes its place; on
# toolchains without one this changes nothing.
NOMANIFEST = $(BUILD)/nomanifest/default-manifest.o

TESTBIN  = $(BUILD)/test_core
PERFTEST = $(BUILD)/test_perf
TESTFLAGS= -std=$(HOSTSTD) $(WARN) -g -fsanitize=address,undefined

.PHONY: all test clean
all: $(BUILD)/wificontrol.exe

$(BUILD):
	@mkdir -p $(BUILD)

$(BUILD)/%.o: src/%.c | $(BUILD)
	$(WINCC) $(WCFLAGS) -c $< -o $@

$(BUILD)/app.res.o: src/app.rc src/resource.h src/app.manifest | $(BUILD)
	$(WINDRES) -I src $< -o $@

$(NOMANIFEST):
	@mkdir -p $(dir $@)
	printf '' | $(WINCC) -c -x c - -o $@

$(BUILD)/wificontrol.exe: $(WOBJ) $(NOMANIFEST)
	$(WINCC) $(WOBJ) -o $@ -B$(dir $(NOMANIFEST)) $(WLDFLAGS) $(WLIBS)
	@echo "built $@ with -std=$(STD) ($$(stat -c %s $@ 2>/dev/null || stat -f %z $@) bytes)"

test: $(TESTBIN) $(PERFTEST)
	./$(TESTBIN)
	./$(PERFTEST)

$(TESTBIN): tests/test_core.c src/wlan_core.c src/wlan.h | $(BUILD)
	$(CC) $(TESTFLAGS) tests/test_core.c src/wlan_core.c -o $@

$(PERFTEST): tests/test_perf.c src/perf.c src/perf.h src/wlan.h | $(BUILD)
	$(CC) $(TESTFLAGS) tests/test_perf.c src/perf.c -o $@

clean:
	rm -rf $(BUILD)

$(BUILD)/app.o:        src/app.h src/perf.h src/perf_win32.h src/ui.h src/wlan.h src/wlan_win32.h
$(BUILD)/app_ui.o:     src/app.h src/ui.h src/tune.h src/wlan.h src/wlan_win32.h src/resource.h
$(BUILD)/ui_draw.o:    src/ui.h
$(BUILD)/ui_ctl.o:     src/ui.h
$(BUILD)/wlan_core.o:  src/wlan.h
$(BUILD)/wlan_win32.o: src/wlan.h src/wlan_win32.h
$(BUILD)/tune.o:        src/tune.h src/wlan.h
$(BUILD)/tune_driver.o: src/tune.h src/wlan.h src/wlan_win32.h
$(BUILD)/tune_power.o:  src/tune.h src/wlan.h
$(BUILD)/tune_ui.o:     src/tune.h src/ui.h src/wlan.h src/wlan_win32.h src/resource.h
$(BUILD)/perf.o:        src/perf.h src/wlan.h
$(BUILD)/perf_win32.o:  src/perf.h src/perf_win32.h src/tune.h src/wlan.h src/wlan_win32.h
