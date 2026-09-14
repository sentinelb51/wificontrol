# wificontrol

A small Windows tray utility that does what [WLANOptimizer][orig] does — ask the
Wi-Fi driver to stop scanning for other access points while you are connected —
without the bugs, and with a UI that tells you whether it actually worked.

Single 90 KB executable. No installer, no service, no runtime, no dependencies
beyond what ships with Windows.

[orig]: https://github.com/catid/WLANOptimizer

## What it changes

Two per-adapter settings, through `WlanSetInterface`:

| Setting | Value | Effect |
| --- | --- | --- |
| `wlan_intf_opcode_background_scan_enabled` | off | stops periodic scans for other APs while associated |
| `wlan_intf_opcode_media_streaming_mode` | on | hints to the driver that latency matters more than power |

Both are reference-counted per client handle and both are reset by Windows the
moment the adapter disconnects. Nothing is written to the registry, nothing
survives a reboot, and **closing this app hands both settings straight back to
Windows** — including if it crashes or is killed. That is the safety model, not
an afterthought.

Whether they measurably help depends on your driver. Some modern drivers ignore
the streaming-mode hint entirely. The Status column tells you what the driver
actually reported back, rather than assuming the write took.

## What is fixed relative to the original

Line references are to `WLANOptimizer.cpp` in the upstream project.

- **Leaked memory on every poll.** `WlanQueryInterface` allocates, and the
  caller must release it with `WlanFreeMemory`. The original never does, on
  either query path (`:69`, `:119`), so it leaks twice per opcode per connected
  adapter every 11 seconds. Freed here in `be_query`/`be_enum`.
- **Short-buffer check that does not match the read.** The original accepts
  `dataSize >= 1` (`:80`, `:130`) and then dereferences a 4-byte `BOOL` through
  it. We require `sizeof(BOOL)`.
- **Raw `BOOL` comparison.** The readback compares `*(BOOL*)dataPtr !=
  targetValue` (`:132`). The API documents *any* nonzero value as TRUE, so a
  driver answering `0xFFFFFFFF` reads as a failure and provokes a rewrite
  forever. We normalise both sides.
- **One error kills the optimizer permanently.** The poll loop `break`s out on
  any unexpected failure (`:340`) and nothing restarts it — silently, with no
  UI. We count consecutive failures per adapter, show them, and keep going.
- **Errors collapse into one global result.** `OptimizeWLAN` overwrites
  `result` per adapter (`:207`, `:218`), so with two adapters you cannot tell
  which one failed. State is tracked per adapter here.
- **11-second polling.** After a reconnect the original leaves up to 11 seconds
  of scanning, and its own README notes the poll itself may cause spikes. We
  register for ACM notifications and re-apply within ~250 ms of a connection
  change, with a 60-second watchdog as a backstop and a re-apply on resume from
  sleep.
- **A one-second blocking call with no thread to run on.** `WlanSetInterface`
  takes about a second and the original holds a global mutex across it. Here it
  runs on a worker thread that owns the WLAN handle; the UI thread never blocks.
- **`ERROR_ACCESS_DENIED` is invisible.** The original returns a code nobody
  sees. This app requests administrator up front so the common case never
  arises, and still probes `WlanGetSecuritySettings` at startup — so if a write
  is refused *even elevated*, it says so and names the likely cause (a group
  policy or a changed Native Wifi DACL) instead of failing silently.

## Stopping all scanning

The **Stop scanning** switch disables Wi-Fi auto configuration
(`wlan_intf_opcode_autoconf_enabled`). Background scan only asks the driver to
stop hunting while associated; this stops the WLAN service scanning at all.

It is off by default, asks before arming, and is the one setting here with real
consequences: **no roaming to a better access point, and no automatic
reconnect if the link drops.**

It also behaves differently from the other two opcodes in a way that dictates
the entire design. Background scan and media streaming mode are reference
counted per client handle and are reset by Windows when the adapter
disconnects, so they cannot get stuck. Auto config has none of that. Nothing
refcounts it, nothing resets it, and it outlives the process — the docs call it
equivalent to `netsh wlan setautoconfig`. Disabled by a program that then dies,
it stays disabled, across reboots, until something puts it back.

So it is put back:

| When | What happens |
| --- | --- |
| The link drops | Re-enabled immediately, so Windows can reconnect |
| You switch it off | Re-enabled |
| The timer expires | Re-enabled, with a notification |
| The master switch goes off | Re-enabled |
| The app exits | Re-enabled before the WLAN handle closes |
| Logging off or shutting down | Re-enabled from `WM_ENDSESSION`, before the reboot |
| The app starts | Re-enabled if anything left it off |

There is no journal and no record of what it used to be. The rule is a single
positive condition — keep it off *only* while armed, enabled, managed and
connected — evaluated against the live value on every pass. Whatever the reason
it is off, including a previous run of this program that was killed, the next
pass turns it back on. That is also why the armed state is deliberately **never
saved to the config file**: the app always starts disarmed, so startup recovery
is unconditional and there is no stored flag that could be wrong.

The arm is time limited by default (15 minutes, or pick another span, or
"until I turn it off"), because the realistic failure is not a crash — it is
arming it and walking away.

The residual risk, stated plainly: if the process is killed outright —
`TerminateProcess`, a power cut — none of the exit paths run, and auto config
stays disabled until you next start the app. Starting it fixes the machine.
Nothing else will.

## Tuning

The **Tuning...** button opens the settings that actually move Wi-Fi latency,
which the two wlanapi opcodes do not touch: the adapter's own advanced
properties and the active power scheme.

Every one of them is a dropdown containing exactly the choices the system
declares, and the rules are the same throughout:

- **Nothing is automatic.** The app never picks a value, never recommends one,
  and never applies anything on its own. You choose, you press Apply.
- **Adapter properties come from the driver.** They are read from its own
  `Ndi\Params` metadata, so whatever your card exposes -- roaming
  aggressiveness, power save mode, scan-when-associated, throughput booster --
  appears under the name the driver gives it. Properties that are a numeric
  range rather than a list of choices are left out; they are not dropdowns.
- **A line on what a well-known setting does.** Microsoft's standardized
  keywords (packet coalescing, ARP/NS offload, wake on pattern, selective
  suspend...), Intel's own properties and both power settings carry a
  one-line technical note: what changing the value does to the radio or the
  link, never which value to pick. Notes are matched on the registry keyword,
  not the display name, so a translated driver still gets them. A property the
  app does not know is shown without one.
- **Only the power settings on the Wi-Fi path.** Wireless Adapter Settings \
  Power Saving Mode, and PCI Express \ Link State Power Management, which is
  what lets an internal Wi-Fi card's link doze between packets. Display,
  processor, GPU and battery policy is not shown. Windows marks ASPM hidden;
  it is listed anyway. A setting this machine does not have is simply absent.
- **No before/after tracking, no undo journal.** The registry and the power
  scheme are the state. Nothing is recorded anywhere about what a value used to
  be. To undo a change, pick the other entry in the same dropdown.
- **Only what you changed is written.** Untouched dropdowns are not rewritten.

Each power setting has two dropdowns, **plugged in** and **on battery**,
because Windows stores them separately and writing one would say nothing about
the other. They take effect immediately. A row you have changed is marked, and
Apply stays disabled until something has been.

Adapter properties do not: the miniport reads them when it starts, so a change
sits in the registry until the device restarts. **Restart adapter** does that
explicitly -- it disables and re-enables the device the way Device Manager does
when you press OK on the Advanced tab, and it drops the link for a few seconds.
It asks first, and it never happens on its own.

## Design notes

- **ACM notifications only.** `WLAN_NOTIFICATION_SOURCE_MSM` additionally
  requires the `wiFiControl` device capability, which since the 2024
  [Wi-Fi/location changes][loc] is gated behind precise-location consent and
  returns `ERROR_ACCESS_DENIED` for an ordinary desktop app. ACM alone reports
  every transition that matters here.
- **No location prompt, ever.** The app deliberately never calls
  `WlanQueryInterface(wlan_intf_opcode_current_connection)`, `WlanScan`,
  `WlanGetAvailableNetworkList` or `WlanGetNetworkBssList` — all of which now
  require precise-location consent, raise a system prompt, and light up the
  location-in-use icon in the tray. Everything shown here comes from
  `WlanEnumInterfaces` and the two BOOL opcodes, none of which are affected.
- **No per-notification-code filtering.** The ACM enumeration is based at
  `L2_NOTIFICATION_CODE_V2_BEGIN`, which the mingw-w64 headers do not define —
  they number it from 0 instead. Comparing against those constants would
  silently match nothing if the real base is nonzero, so every ACM notification
  triggers one debounced, rate-limited poll instead. A poll that finds nothing
  to change costs two `WlanQueryInterface` calls per adapter.
- **The policy layer knows nothing about Windows.** `src/wlan_core.c` talks to
  a four-function backend, so all the decision-making is unit-tested on the
  build host under ASan/UBSan. Each test in `tests/test_core.c` pins one of the
  defects listed above.

[loc]: https://learn.microsoft.com/en-us/windows/win32/nativewifi/wi-fi-access-location-changes

## Using it

The app manifest requests administrator, because `WlanSetInterface` on these
two opcodes is gated by the Native Wifi securable objects and a standard user
is normally refused. So there is **one UAC prompt each time it starts**, and no
permission problems after that.

Run it. Each adapter gets a card showing its connection and what the driver
reported back for each setting, ticked where it matches what was asked for;
switch **Manage** off on a card to leave that adapter alone. **Optimize** turns
everything off and hands the settings back without quitting. Closing the window
hides it to the tray — the settings only last while the process is alive — and
Exit in the tray menu really quits.

The window is dark by default. **Dark theme** in the tray menu switches it to
light and back, and the choice is saved. Popup menus follow along on Windows 10
1903 and later through an undocumented uxtheme export, guarded by build number;
the two confirmation prompts are standard message boxes and stay light.

### Starting it automatically

An app that requires administrator **cannot** be launched from the Startup
folder or a `Run` key: Windows will not raise a UAC prompt at logon, so the
entry is silently skipped. Use a scheduled task with highest privileges
instead, which starts it elevated with no prompt at all:

```
schtasks /create /tn WifiControl /sc onlogon /rl highest /f ^
         /tr "\"C:\path\to\wificontrol.exe\" /tray"
```

`/tray` starts it hidden in the notification area. Remove it again with
`schtasks /delete /tn WifiControl /f`.

### Where settings live

`%LOCALAPPDATA%\WifiControl\wificontrol.ini`, or a `wificontrol.ini` next to
the executable if you create one there first (portable mode). Nothing else is
written. Note that if you elevate using a *different* administrator account,
`%LOCALAPPDATA%` resolves to that account's profile.

## Building

Needs mingw-w64; nothing else. Builds from Linux, macOS or MSYS2.

```
make          # build/wificontrol.exe
make test     # run the core tests natively under ASan/UBSan
```

The Makefile probes for the newest C standard each compiler accepts —
`-std=c23` on GCC 14+, `-std=c2x` on GCC 13 and earlier, which is the same
language under the older spelling. Override with `make STD=c17` if you need to.

Built `-O3 -flto` with `-ffunction-sections`/`--gc-sections`. Nothing is
allowed to add instructions to the hot path: `-fno-stack-protector` (no canary
load and compare per frame) and `-fcf-protection=none` (no `endbr64` at every
indirect branch target). The resulting binary contains zero of either.

`--dynamicbase --nxcompat --high-entropy-va` stay on. Those are PE header bits
and load-time relocations — they execute nothing and cost no CPU, and without
them the binary is the kind of thing SmartScreen and AV heuristics flag.

`make OPT=-Os` for the smallest build. `make ARCH=x86-64-v2` (SSE4.2, ~2009+)
or `ARCH=x86-64-v3` (AVX2, ~2013+) if you only ever run it on your own
machines; the default baseline runs anywhere, and an older CPU will fault on an
instruction it does not have.

## Layout

| File | |
| --- | --- |
| `src/wlan.h`, `src/wlan_core.c` | policy: what to apply, when to retry, how to read an error |
| `src/wlan_win32.c` | the only file that touches wlanapi |
| `src/app.c` | config, icon, worker thread, startup |
| `src/app_ui.c` | the main window: switches, adapter cards, tray |
| `src/ui.h`, `src/ui_draw.c`, `src/ui_ctl.c` | theme, anti-aliased drawing, owner-drawn switches and buttons, scrolling panel |
| `src/tune.h`, `src/tune.c` | the settings model, and writing only what changed |
| `src/tune_driver.c` | adapter advanced properties, from the driver's `Ndi\Params`, and notes on well-known ones |
| `src/tune_power.c` | the two Wi-Fi-path power settings in the active scheme |
| `src/tune_ui.c` | the tuning window |
| `tests/test_core.c` | regression test per fixed defect and per recovery path |
| `planned/monitoring.md` | design for the latency/signal monitor; not implemented |
