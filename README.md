# wificontrol

A small Windows tray utility that does what [WLANOptimizer][orig] does — ask the
Wi-Fi driver to stop scanning for other access points while you are connected —
without the bugs, and with a UI that tells you whether it actually worked.

Single 75 KB executable. No installer, no service, no runtime, no dependencies
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

Run it. Adapters are listed with a checkbox each; uncheck one to leave it
alone. The master checkbox turns everything off and hands the settings back
without quitting. Closing the window hides it to the tray — the settings only
last while the process is alive — and Exit in the tray menu really quits.

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

Built `-O3 -flto` with `-ffunction-sections`/`--gc-sections`, plus
`-fstack-protector-strong`, `-fcf-protection=full` (Intel CET) and
`--dynamicbase --nxcompat --high-entropy-va`, so the binary ships with ASLR,
DEP and high-entropy 64-bit relocation. Override the two flag groups with
`make OPT=-Os HARDEN=` if you want the smallest possible build instead — it
saves about 2 KB, and nothing in this program is hot enough for `-O3` to
matter otherwise.

## Layout

| File | |
| --- | --- |
| `src/wlan.h`, `src/wlan_core.c` | policy: what to apply, when to retry, how to read an error |
| `src/wlan_win32.c` | the only file that touches wlanapi |
| `src/app.c` | window, list, tray, DPI, config, worker thread |
| `tests/test_core.c` | regression test per fixed defect |
