# ESP32U analyzer management plane v1

**Status: v1 protocol specification; implementation pending.** The firmware
currently running on the ESP32U still speaks the existing `iq_*` commands and
WJIQ v2. This specification does not claim that the commands below already
work. The separate [sample-plane proposal](planes-v1-proposal.md) is not yet
finalized or implemented.

The management plane configures an IQ capture device. Rates describe sample
spacing *within a capture*, not a promise of continuous samples or delivery
throughput. The ESP32U has gaps between finite captures. Frame-event and
promiscuous-callback control are outside this protocol.

## Transport and framing

- UDP/IPv4, device destination port **22**. A client sends to the device's
  unicast IPv4 address; responses go to the request's source IP and port.
- One command per datagram; ASCII text, optional final LF or CRLF. A request
  and its complete single-datagram response each fit in 512 bytes. Reject
  longer or multi-line requests; never execute a truncated request.
- Correlated requests use `cmd:<id> <command> [arguments]`, where `id` is an
  unsigned decimal 8-bit integer (0–255). Responses are `OK:<id> <body>\n` or
  `NOK:<id> <code>\n`. ID correlates the reply; it is not an operation ID or
  replay-protection token. A malformed prefix returns uncorrelated `NOK EINVAL`.
- For compatibility with the existing winject convention, a request without
  `cmd:<id>` is also accepted and returns `OK <body>\n` or `NOK <code>\n`.
  Clients should use correlation. Command names and prefixes are ASCII
  case-insensitive. Whitespace is one or more spaces or tabs; numeric arguments
  contain decimal digits only, with no sign, suffix or unit. Extra, missing or
  malformed arguments are rejected; no command is partially applied.
- A client must use only one outstanding command per `(device IP, source port,
  request ID)` and check the response source address, source port and ID.
  Do not reuse an ID while a response for it may still be in flight.

UDP can lose, delay, duplicate or reorder packets. Getters, each setter, and
start/stop are idempotent when repeated against unchanged intervening state.
A client may retry after a timeout **only if it has not issued an intervening
conflicting command**. Retrying a delayed `start_samples` after a subsequent
`stop_samples`, or a setter after a different setter, can change the state
again. There is no built-in multi-controller arbitration: operators coordinate
controllers and clients serialize state-changing commands per device.
Management is unauthenticated and intended for a trusted LAN.

## Device configuration

The configuration applies globally to subsequent captures. The sample group
is `239.255.53.1`, UDP destination port `5300`, IPv4 multicast TTL 1. These
are fixed in v1 and reported by the device; there is no multicast setter in
this version. The sample-plane format and its on-wire version are defined
separately and must be advertised truthfully by `get_sample_config`.

| Field | Unit and v1 behavior | Initial default |
| --- | --- | --- |
| `freq_hz` | Exact center frequency, unsigned 64-bit Hz. Only frequencies advertised by `get_sample_caps` are accepted. The present tuning backend represents whole MHz. | `2437000000` |
| `rate_hz` | Complex samples per second within a capture. Current backend: `16000000`, `40000000`, `80000000`. | `16000000` |
| `depth` | Complex samples per capture, inclusive current backend range `256`–`16380`, step `1`. | `16380` |
| `gain_mode` | `agc` enables hardware automatic gain; `manual` forces `gain_index`. | `agc` |
| `gain_index` | Configured manual gain index, unsigned integer from 0 through the runtime `gain_index_max`. This is an index, not dB. | `min(40, gain_index_max)` |
| `bandwidth_hz` | Requested analog low-pass width in Hz. `0` selects the PHY default; otherwise integer MHz in the inclusive range 12–67 MHz for the current backend. | `0` |
| `format` | `iq8_i_qraw`: signed interleaved I and Qraw bytes; conventional complex is I − j·Qraw. | fixed in v1 |

A successful setter changes exactly one field or does nothing when the value
is already applied. It must not silently clamp or round. Unsupported and
out-of-range arguments return `EINVAL`; unsupported device capabilities are
reported by `get_sample_caps`, not guessed by clients. The receiver's tune
acceptance range does not establish calibrated RF response or legal operation
throughout that range. Frequency values over 4.29 GHz must also be parsed and formatted without
32-bit overflow.

Mode argument values `agc` and `manual` are ASCII case-insensitive and are
always reported lower-case. `gain_mode=manual` applies the saved `gain_index`;
`gain_mode=agc` leaves
hardware AGC in control. `set_gain_index` updates the configured manual index
in either mode: in AGC mode it is retained but does not force gain until a
subsequent switch to manual. Switching back to AGC preserves that configured
index. The configured index is not a reading of the gain chosen by AGC.
`gain_index_max` is read from the device's hardware during initialization and
must be queried; 81 was observed on one ESP32U, not guaranteed on all boards.
The backend internally clamps indices, so the m-plane must validate the value
before calling it and return `EINVAL` rather than silently clamping.

`bandwidth_hz=0` requests the PHY's default low-pass setting. Nonzero values
must be exact whole MHz, 12000000–67000000 inclusive. The backend maps each
requested integer MHz through a quantized hardware filter control; the value
returned is the **requested setting**, not a measured passband. The two-sided
usable width is approximate and depends on calibration and hardware; do not
claim it is an exact 3 dB bandwidth. The setting is applied around each
capture and the hardware filter registers are restored afterward. The mapping
and resulting frequency response need calibration before precision claims.

IQ8 is fixed for the proposed new sample plane. The existing legacy WJIQ v2
also implements packed IQ10; that does not make IQ10 available in the new
sample-plane format, and this m-plane version has no IQ10 setter.

Configuration changes are accepted **only in `stopped` state**; otherwise a
setter returns `EBUSY`, including a setter repeating the current value. A
successful non-no-op setter increments `revision` once modulo 2^32; a no-op
returns the current revision. `revision` starts at 1 on each boot. v1 applies
settings in RAM only: they are not saved across reboot, and defaults apply
again. There is no atomic multi-field transaction; after several setters,
query configuration before starting.

## Commands

All successful replies begin with the exact body name shown below, followed
by space-separated `key=value` tokens. A client must accept fields in any
order and ignore unknown keys for forward compatibility, but must validate
required fields. `boot_id` is a random unsigned 64-bit number chosen at boot;
it helps detect reboots but is not a security token and can theoretically
collide. Every `revision` is scoped to that boot ID.

| Request | Success body | Effect |
| --- | --- | --- |
| `set_center_freq_hz <freq>` | `set_center_freq_hz freq_hz=<u64> revision=<u32>` | Set exact frequency in Hz. |
| `set_complex_sample_rate <rate>` | `set_complex_sample_rate rate_hz=<u32> revision=<u32>` | Set exact complex-sample rate. |
| `set_sample_depth <depth>` | `set_sample_depth depth=<u32> revision=<u32>` | Set samples per capture. |
| `set_gain_mode <agc|manual>` | `set_gain_mode gain_mode=<agc|manual> revision=<u32>` | Enable AGC or apply the configured manual gain index. |
| `set_gain_index <index>` | `set_gain_index gain_index=<u32> revision=<u32>` | Select the manual gain index; while in AGC mode store it for later use. |
| `set_analog_bandwidth_hz <width>` | `set_analog_bandwidth_hz bandwidth_hz=<u32> revision=<u32>` | Set approximate analog low-pass width; `0` selects PHY default. |
| `get_sample_config` | `sample_config ...` | Read the complete current configuration and state. |
| `get_sample_caps` | `sample_caps ...` | Read device limits and protocol capabilities. |
| `start_samples` | `start_samples state=running revision=<u32> boot_id=<u64>` | Start continuous *repeated captures*. |
| `stop_samples` | `stop_samples state=stopped revision=<u32> boot_id=<u64>` | Stop capture and submit no more sample datagrams from that run. |
| `get_sample_status` | `sample_status ...` | Read state and counters. |
| `get_time_info` | `time_info ...` | Read sample timestamp validity and clock semantics. |

Required `sample_config` fields:

```
boot_id=<u64> revision=<u32> state=<stopped|running> mplane_version=1 splane_version=<u32> group=239.255.53.1 port=5300 ttl=1 format=iq8_i_qraw freq_hz=<u64> rate_hz=<u32> depth=<u32> gain_mode=<agc|manual> gain_index=<u32> bandwidth_hz=<u32>
```

`state` and all configuration fields in one response must be a coherent
snapshot. `splane_version` identifies the format actually emitted, not a
proposed version. A host must reject an unfamiliar sample-plane version. The
sample plane remains separately undecided; this m-plane specification does not
make WJIQ v2 compatible with a proposed new packet format.

Required `sample_caps` fields:

```
mplane_version=1 freq_min_hz=<u64> freq_max_hz=<u64> freq_step_hz=<u64> rates_hz=16000000,40000000,80000000 depth_min=256 depth_max=16380 depth_step=1 formats=iq8_i_qraw gain_modes=agc,manual gain_index_min=0 gain_index_max=<u32> gain_index_step=1 bandwidth_auto=1 bandwidth_min_hz=12000000 bandwidth_max_hz=67000000 bandwidth_step_hz=1000000
```

`freq_min_hz`, `freq_max_hz` and `freq_step_hz` describe the actual accepted
inclusive arithmetic progression on that firmware: `freq_min_hz + k *
freq_step_hz`, not exceeding `freq_max_hz`. A firmware with a different set
of frequencies or rates must advertise it accurately, or introduce an
extension/version rather than accepting values it cannot report. The rate list
is comma-separated, without whitespace; the listed values above are the
current backend's intended capabilities. `gain_index_max` is the runtime
maximum. `bandwidth_auto=1` means value zero selects the PHY default; the
bandwidth min/max/step describe accepted requested values, not measured analog
frequency response. An implementation whose supported gain or filter controls
differ must advertise the actual values.

Required `sample_status` fields:

```
boot_id=<u64> revision=<u32> state=<stopped|running> captures=<u32> capture_errors=<u32> datagrams_sent=<u32> send_errors=<u32> uptime_us=<u64>
```

Counters are monotonic modulo 2^32 since boot, and are not cleared by start,
stop or getters. `captures` counts successful hardware captures; capture
failures increment `capture_errors`. `datagrams_sent` counts sample UDP
datagrams accepted by the network driver, not those received by listeners;
failed sample datagram submissions increment `send_errors`. Compare counter
deltas modulo 2^32 only when `boot_id` is unchanged. `uptime_us` is the
monotonic device uptime, not UTC.

Required `time_info` fields:

```
sample0_valid=<0|1> clock=<unknown|device_monotonic> resolution_ns=<u64> accuracy_ns=<u64>
```

Initially return `sample0_valid=0 clock=unknown resolution_ns=0 accuracy_ns=0`
until the sample-zero reference and its uncertainty are measured. When valid,
resolution and an empirically justified error bound must be nonzero and the
clock is monotonic since this `boot_id`; nanosecond units alone imply no
precision. This command does not claim synchronization with the OPi5 or UTC.

## Start, stop and state

`start_samples` applied in `stopped` state returns `OK` only after the stream
has been accepted and state is `running`; a repeated start while running
returns the same successful state without restarting the stream or resetting
counters. `stop_samples` returns `OK` in `stopped` state and is a no-op. From
`running`, stop finishes or abandons the active burst and returns `OK` only
when no more datagrams from that run will be submitted to the network driver;
state then is `stopped`. A driver or network can still deliver already queued
datagrams after the stop reply. A receiver must discard stale packets when
changing configuration and cannot use the stop reply as a network-delivery
barrier.

`get_sample_config` and `get_sample_status` report only `stopped` or `running`
in v1. If the device cannot complete a transition it returns `EIO` and its
status must reflect the resulting stable state. A stop can take longer than a
normal getter: clients use a timeout of at least 2 seconds and may retry under
the ordering rule above. On boot, state is `stopped`.

## Errors

| Code | Meaning |
| --- | --- |
| `EINVAL` | Malformed command, wrong number of arguments, unsupported value or out-of-range value. |
| `ENOSYS` | Unknown command. |
| `EBUSY` | A setter was requested while samples are running. |
| `ENODEV` | The receiver is not available. |
| `EIO` | A hardware or internal operation failed. |

A failed setter leaves its field and `revision` unchanged; `EIO` must not
report an applied value that is uncertain. If the implementation cannot
restore the previous hardware state, it reports `ENODEV` on subsequent
operations until reinitialized, rather than claiming the old configuration is
still active. Failed start/stop and lost responses require querying status;
the client must not infer state from a timeout.

## Example

```
> cmd:1 set_center_freq_hz 2437000000
< OK:1 set_center_freq_hz freq_hz=2437000000 revision=1
> cmd:2 set_complex_sample_rate 16000000
< OK:2 set_complex_sample_rate rate_hz=16000000 revision=1
> cmd:3 set_sample_depth 16380
< OK:3 set_sample_depth depth=16380 revision=1
> cmd:4 get_sample_config
< OK:4 sample_config boot_id=9123456789 revision=1 state=stopped mplane_version=1 splane_version=1 group=239.255.53.1 port=5300 ttl=1 format=iq8_i_qraw freq_hz=2437000000 rate_hz=16000000 depth=16380 gain_mode=agc gain_index=40 bandwidth_hz=0
> cmd:5 start_samples
< OK:5 start_samples state=running revision=1 boot_id=9123456789
> cmd:6 get_time_info
< OK:6 time_info sample0_valid=0 clock=unknown resolution_ns=0 accuracy_ns=0
> cmd:7 stop_samples
< OK:7 stop_samples state=stopped revision=1 boot_id=9123456789
> cmd:8 set_gain_index 40
< OK:8 set_gain_index gain_index=40 revision=1
> cmd:9 set_gain_mode manual
< OK:9 set_gain_mode gain_mode=manual revision=2
> cmd:10 set_analog_bandwidth_hz 20000000
< OK:10 set_analog_bandwidth_hz bandwidth_hz=20000000 revision=3
```

The example assumes `gain_index_max >= 40`. It uses `splane_version=1`
illustratively; an implementation must
report its actual sample-plane version. Start and stop replies describe the
local device state, not successful reception by every multicast subscriber.
