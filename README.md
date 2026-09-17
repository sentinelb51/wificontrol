# Introduction

### Purpose
WiFi Control is a Windows tray utility that stops your Wi-Fi driver from scanning for other access points while you're connected.
Those scans are what cause the periodic latency spikes in games and calls.

### Context
It does what [WLANOptimizer](https://github.com/catid/WLANOptimizer) does, minus the bugs. WLANOptimizer leaks memory on every poll,
dies silently on the first unexpected error, leaves up to 11 seconds of scanning after every reconnect, and never tells you
whether any of it worked. This one re-applies within ~250 ms of a reconnect and shows you what the driver reported back.

### Platforms
Windows only, x64. It has only been tested on Windows 11.

One ~130 KB executable; no installer, no service, no runtime, nothing beyond what ships with Windows.

### Disclaimer
Whether it measurably helps depends on your driver; some ignore the streaming-mode hint entirely.
The cards show what the driver reported back, not what the app asked for.

# Features

### Latency
- **No background scans, and streaming mode**; two switches, set per adapter through `WlanSetInterface`
- **Re-applied within ~250 ms of a reconnect**; driven by connection events, with a 60-second watchdog and a re-apply on resume from sleep
- **Never blocks the UI**; the slow (~1 second) driver call runs on its own worker thread
- **Honest status**; each card shows what the driver actually reported, and failures are counted and retried instead of silently stopping

### Safety
- **Nothing gets stuck**; Windows resets the two base settings the moment the app exits, even if it crashes or is killed
- **The other switches undo themselves**; when switched off, on exit, and on the next start if a previous run was killed
- **No location prompt, ever**; the app never calls the Wi-Fi APIs that need location consent, so the location icon stays off
- **Access denied is explained**; if a write is refused even as administrator, it says so and names the likely cause

### Switches
Independent of each other; each applies to every adapter whose card has **Manage** on.
- **No background scans** (on by default); Windows stops its once-a-minute scan for other networks while you're connected.
  Scans you or other apps ask for, such as opening the Wi-Fi list, still happen
- **Streaming mode** (on by default); tells the driver latency matters more than power. Some drivers ignore it
- **Block all scans** (off by default); turns off Wi-Fi auto configuration, which also blocks the scans you or other apps ask for. **No roaming and no automatic reconnect** while it's on.
  It asks first, starts off every time, and gives scanning back when the link drops, when the timer runs out (15 minutes by default), and on exit.
  If the app is killed outright, scanning stays off until you start it again
- **Metered** (on by default); marks your saved Wi-Fi networks as pay-per-byte, so Windows Update, Delivery Optimization, OneDrive and the Store stop downloading in the background.
  It overrides any network you marked as metered yourself
- **Performance** (off by default); turns off power saving on the Wi-Fi path: MIMO power save, U-APSD, selective suspend and PCIe link state power management, with transmit power and the power plan's Wi-Fi setting at maximum.
  It also disables the Wi-Fi Direct adapters, so Miracast, Mobile Hotspot and Wi-Fi Direct stop working.
  The adapter restarts to apply it (Wi-Fi drops for a few seconds), and battery life suffers. The old values are saved first and put back when you're done

### Tuning
- **Driver advanced properties**; read from the driver itself, so you get whatever your card exposes, under the names it gives them
- **Wi-Fi power settings**; adapter power saving mode and PCIe link state power management, plugged in and on battery
- **Wi-Fi Direct toggle**; one dropdown that disables the virtual adapters behind Miracast, Mobile Hotspot and Wi-Fi Direct
- **One-line notes**; what well-known settings do to the radio or the link, never which value to pick
- **Nothing automatic**; you pick, you press Apply, and only what you changed gets written

### Interface
- **Dark by default**; untick **Dark theme** in the tray menu for light
- **Lives in the tray**; closing the window hides it, because the settings only last while the app is running.
  The tray menu toggles **No background scans**, **Streaming mode** and **Performance**

# Getting started

## Installation
Download `wificontrol.exe` from the **[latest release](https://github.com/sentinelb51/wificontrol/releases/latest)** and put it wherever you like.
Every push to `main` replaces it, so that link always has the newest build.

## Usage
Run it and accept the UAC prompt. Windows refuses these settings to standard users, so it needs administrator;
that's one prompt per start, and no permission problems after that.

Each adapter gets a card with its connection and what the driver reported for each setting.
Switch **Manage** off on a card to leave that adapter alone. Turning a switch off hands its setting back without quitting,
and every switch except **Block all scans** is remembered. **Exit** in the tray menu actually quits, and puts everything back.

### Starting with Windows
Apps that need administrator **can't** start from the Startup folder or a `Run` key; Windows silently skips them at logon.
Use a scheduled task instead, which starts it elevated without a prompt:

```cmd
schtasks /create /tn WifiControl /sc onlogon /rl highest /f /tr "\"C:\path\to\wificontrol.exe\" /tray"
```

`/tray` starts it hidden. To remove it, run `schtasks /delete /tn WifiControl /f`

### Settings
Stored in `%LOCALAPPDATA%\WifiControl\wificontrol.ini`. For portable mode, create an empty `wificontrol.ini` next to the executable.

Don't delete `wificontrol-restore.ini` while **Performance** is on; it holds the values that switch puts back.

## Building
Needs mingw-w64, nothing else. Builds on Windows, Linux or macOS.

```bash
make        # build/wificontrol.exe
make test   # unit tests under ASan/UBSan
```

The decision-making (`src/wlan_core.c`, `src/perf.c`) never sees Windows, so it's unit-tested on any host.

## Resource usage
Sitting in the tray, it uses:

- ~0.00% CPU
- ~3 MB of RAM
