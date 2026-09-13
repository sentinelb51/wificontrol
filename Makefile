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
OPT     ?= -O3 -flto=auto -fuse-linker-plugin -fno-ident
HARDEN  ?= -fstack-protector-strong -fcf-protection=full
WCFLAGS  = -std=$(STD) $(WARN) $(OPT) $(HARDEN) -municode -D_WIN32_WINNT=0x0601 \
           -ffunction-sections -fdata-sections
WLDFLAGS = -municode -mwindows $(OPT) -Wl,--gc-sections -s \
           -Wl,--dynamicbase -Wl,--nxcompat -Wl,--high-entropy-va
WLIBS    = -lwlanapi -lcomctl32 -lshell32 -lgdi32 -luser32

WSRC     = src/app.c src/wlan_core.c src/wlan_win32.c
WOBJ     = $(patsubst src/%.c,$(BUILD)/%.o,$(WSRC)) $(BUILD)/app.res.o

TESTBIN  = $(BUILD)/test_core
TESTFLAGS= -std=$(HOSTSTD) $(WARN) -g -fsanitize=address,undefined

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
	@echo "built $@ with -std=$(STD) ($$(stat -c %s $@ 2>/dev/null || stat -f %z $@) bytes)"

test: $(TESTBIN)
	./$(TESTBIN)

$(TESTBIN): tests/test_core.c src/wlan_core.c src/wlan.h | $(BUILD)
	$(CC) $(TESTFLAGS) tests/test_core.c src/wlan_core.c -o $@

clean:
	rm -rf $(BUILD)

$(BUILD)/app.o:        src/wlan.h src/wlan_win32.h src/resource.h
$(BUILD)/wlan_core.o:  src/wlan.h
$(BUILD)/wlan_win32.o: src/wlan.h src/wlan_win32.h
