# Planned: monitoring

Not implemented. This is the design for a global monitoring toggle that answers
one question: *is any of this actually doing anything?*

Everything WLAN Optimizer and its descendants do is taken on faith. The point
of monitoring is to replace faith with a number you can watch change when you
flip a switch.

## Scope

A single **Monitoring** checkbox, off by default. While it is on:

- **Latency** to the default gateway: rolling average, min, max, p99, loss
- **Signal**: RSSI in dBm, link rate, and on Windows 11 the real-time
  connection quality

While it is off, nothing is sampled, no thread runs, and no timer is armed.

## Non-goals

Stated up front because they shape the whole design:

- **No history. No trails.** Nothing is retained per sample. No ring buffer of
  measurements, no time series, no sparkline, no graph.
- **Nothing written to disk.** Statistics live in memory and die with the
  process.
- **No automatic action.** Monitoring never changes a setting. It reports.

## The hard part: p99 without keeping samples

Average, min and max are free — a running sum and two comparisons, O(1) memory,
no samples retained. A percentile normally is not: the textbook method sorts
the samples, which means keeping them, which is exactly the trail we said we
would not keep.

Two ways out, neither of which stores a sample:

**Fixed-bucket histogram (recommended).** Pre-declare ~64 log-spaced latency
buckets covering roughly 0.5 ms to 4 s. Each sample increments one `uint32`
counter. p99 is read back by walking the counts to the 99th-percentile
position. Fixed ~256 bytes, O(1) per sample, no allocation, trivially correct,
and every other quantile comes out of the same structure for free.

The honest cost: p99 is an *estimate*, accurate to the width of the bucket it
lands in. With log spacing that is a few percent of the value, which is far
finer than the thing being measured varies anyway. The UI should therefore
render it as `p99 ~12 ms`, not `p99 12.00 ms`.

**P² (Jain & Chlamtac)** estimates a single quantile from five running markers
with no buckets at all. Smaller and more elegant, but it gives one quantile per
instance, is fiddly to implement correctly, and degrades on strongly bimodal
input — which Wi-Fi latency, the thing we are specifically trying to observe,
absolutely is. The histogram is the better fit here.

Decision: histogram.

## Sampling latency

`IcmpSendEcho2` from `iphlpapi`. No raw sockets, no winsock setup, and the
outbound ICMP echo is not subject to the inbound firewall rules that make raw
sockets awkward.

Target the **default gateway**, not a public address. The whole question is
whether the Wi-Fi hop stalls; routing the probe through the ISP adds variance
that has nothing to do with the adapter. Gateway comes from
`GetAdaptersAddresses(AF_INET, GAA_FLAG_INCLUDE_GATEWAYS)`, matching
`IfIndex`/adapter GUID to the adapter selected in the main list.

One probe per second by default. Note in the UI that the probe is itself
traffic, and that a monitored idle link is not quite an idle link.

A timeout counts as loss and feeds the loss ratio; it does not feed the
latency histogram.

## Sampling signal

Both of these are safe to call and, importantly, neither is affected by the
2024 Wi-Fi/location changes, so monitoring must never trigger a location
prompt:

- `wlan_intf_opcode_rssi` — `LONG`, dBm
- `wlan_intf_opcode_realtime_connection_quality` — Windows 11, gives link
  quality and rate while explicitly omitting location-sensitive fields

`wlan_intf_opcode_current_connection` is **not** used, for exactly that reason.
It is the obvious place to get signal quality from and it is the one thing that
would put a location-in-use icon in the tray.

Signal is a gauge, not a distribution: show the current value plus min/max over
the window. It does not need the histogram.

## Resetting

Statistics are cumulative since the window opened. They reset when:

- monitoring is toggled off and on again
- the monitored adapter disconnects or changes
- the user presses **Reset**

That last one is the one that matters: the intended workflow is reset, watch,
flip a setting, reset, watch again, compare. Since nothing is stored, comparing
means writing the first number down — which is fine, and considerably more
honest than a graph implying precision that a bucketed p99 does not have.

## Threading

`IcmpSendEcho2` blocks for up to the timeout, so it cannot run on the UI thread
and must not run on the WLAN worker thread — a probe stalling for a second
would delay re-applying settings after a reconnect.

A third thread, owned by the monitor, sampling on its own timer, publishing a
snapshot of the statistics to the UI by `PostMessage` the same way the WLAN
worker already does. Same no-locks pattern as the rest of the app.

The thread exists only while monitoring is on.

## Open problem: counting scans

The most direct evidence that disabling background scan worked is the scan rate
itself — count `wlan_notification_acm_scan_complete` per minute with the
setting on and off, and watch it fall to zero.

This is currently blocked. As noted in the README, the app does not filter ACM
notifications by code, because the mingw headers number the ACM enumeration
from 0 while the SDK bases it at `L2_NOTIFICATION_CODE_V2_BEGIN`, which is not
defined in either header available here. Counting a *specific* notification
requires knowing that base.

It is determined in one step on real hardware: log the raw `NotificationCode`
of every ACM notification for a minute, connect and disconnect once, and read
the base off the values. Until someone does that, this stays unimplemented
rather than guessed at — a scan counter that silently counts nothing would be
worse than no scan counter.

## UI sketch

A group box under the adapter list, visible only while monitoring is on:

```
Gateway 192.168.1.1     avg 3.2 ms   min 1.8   max 41   p99 ~12   loss 0.0%
Signal  -52 dBm         min -58  max -49       link 866 Mbps          [Reset]
```

One line each, no chrome. If it needs a graph it has stopped being this
feature.
