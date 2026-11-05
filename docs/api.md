# Analyzer API

The device serves the web application and API from the same Ethernet origin.
This avoids browser access to raw UDP sockets and keeps CORS out of the
firmware. Automation can use either HTTP/WebSocket or the UDP mplane mirror.

## Receiver modes and ESP32 limits

The original ESP32 cannot run its normal WiFi MAC receiver and the ESP-SDR RF
dump path as one coherent, lossless receiver. The API exposes the active mode
rather than pretending that every measurement is simultaneous.

| Mode | Source | Output |
| --- | --- | --- |
| `frames` | ESP-IDF promiscuous WiFi RX | Full received MPDU, `wifi_pkt_rx_ctrl_t` metadata, RSSI, optional CSI |
| `iq` | ESP-SDR RF dump | Raw IQ8/IQ10 snapshots, unstructured/tone power, FFT |
| `sweep` | Repeated ESP-SDR tune + FFT snapshots | Spectrum tiles over the requested range |

CSI is tied to a received WiFi frame and contains complex channel estimates
per subcarrier. It is useful as a constellation-like channel plot, but it is
not the transmitted data-symbol constellation. Raw ESP-SDR I/Q is true
baseband, but a snapshot cannot be asserted to belong to the same MPDU reported
by the WiFi MAC. The web app labels these two views `Frame CSI` and `Raw IQ`.

## Implementation status

| Area | Status |
| --- | --- |
| Web app at `/` (embedded in flash), OTA form moved to `/ota` | done |
| `GET /api/v1/info`, `GET`/`PATCH /api/v1/config` (frames mode) | done |
| WebSocket `frame`, `frame.csi`, `power` (`rssi`), `dropped` | done |
| `iq` / `sweep` modes, `POST /captures`, `POST /sweeps`, `DELETE /operations/*` | answer `501` until the ESP-SDR receiver lands |
| UDP mplane `analyzer_*` mirror | planned; `radio_tx channel=` already retunes over mplane |

`info.features` lists only what the running firmware implements; the web app
disables views whose feature is missing. `info.planned` names the rest.

## HTTP

All JSON endpoints are under `/api/v1`.

### `GET /api/v1/info`

Returns immutable capabilities and live identity:

```json
{
  "api": 1,
  "name": "winject-wifi-analyzer-esp32u",
  "mode": "frames",
  "frequency_mhz": 2442,
  "tuning": {"min_mhz": 100, "max_mhz": 6000, "measured": [[1850, 2850]]},
  "rates": [80000000, 40000000, 16000000],
  "sample_bits": [8, 10],
  "max_samples": 16380,
  "features": ["frames", "csi", "iq", "power", "sweep"]
}
```

The broad tuning limits are those advertised by ESP-SDR. `measured` is the
range in which ESPARGOS reports over-the-air reception on the original ESP32;
the UI warns when a sweep leaves it.

### `GET /api/v1/config`

Returns the active mode, center frequency, gain and mode-specific settings:

```json
{"mode":"frames","epoch":2,"channel":6,"frequency_mhz":2437,
 "gain":{"mode":"hardware"},"csi":true,
 "filter":{"types":["mgmt","data","misc"],"min_rssi":-128,"fcs":"any","mac":null}}
```

`filter` applies on the device before frames are queued: `types` (any of
`mgmt`, `ctrl`, `data`, `misc`; `ctrl` adds ACK/RTS/CTS traffic), `min_rssi`
(dBm), `fcs` (`ok` drops FCS failures, `any` keeps them) and `mac` (match on
Addr1, Addr2 or Addr3; `null` disables).

### `PATCH /api/v1/config`

Applies an atomic partial update. Example:

```json
{"mode":"iq","frequency_mhz":2442,"gain":{"mode":"manual","index":40}}
```

Changing mode stops the current frame stream, capture or sweep and increments
the stream epoch; so does retuning. The whole patch is validated before any of
it is applied. In `frames` mode, `frequency_mhz` (or `channel`) must be a WiFi
channel center (2412–2472 step 5, 2484 MHz) and `gain` must be
`{"mode":"hardware"}`; manual gain is an ESP-SDR setting. Errors answer
`{"error":"..."}` with 400, 409 (radio refused the channel) or 501.

### `POST /api/v1/captures`

Starts one bounded raw capture and returns `202 Accepted`:

```json
{"samples":16380,"sample_rate_hz":16000000,"bits":8}
```

The result arrives as `capture.meta` followed by binary `WJIQ` fragments on
the WebSocket. The CRC and fragment semantics are defined in
[sample-protocol.md](sample-protocol.md).

### `POST /api/v1/sweeps`

Starts a cancellable sweep:

```json
{
  "start_mhz":1850,
  "stop_mhz":2850,
  "step_mhz":10,
  "fft_bins":512,
  "sample_rate_hz":16000000,
  "detector":"average",
  "dwell_captures":4
}
```

Each completed tuning step emits `sweep.tile`; progress and the final state are
JSON events. A sweep is rejected when another IQ operation owns the receiver.

### `DELETE /api/v1/operations/{id}`

Cancels a capture or sweep. Cancellation is cooperative between RF snapshots.

## WebSocket

Connect to `/api/v1/events`. Text messages are JSON event envelopes:

```json
{"event":"frame","epoch":12,"sequence":91,"time_us":442019,"data":{}}
```

Events:

| Event | Important fields |
| --- | --- |
| `frame` | RSSI, channel, rate/MCS flags, FCS state, addresses, frame-control, full MPDU as base64 |
| `frame.csi` | Frame sequence, signed CSI I/Q pairs and CSI metadata |
| `power` | Source (`rssi` or `iq`), dBFS/RSSI, peak, RMS, clipping, calibration state |
| `capture.meta` | Capture ID, format, total bytes and CRC32; binary fragments follow |
| `sweep.tile` | Operation ID, center/range, bin width and dBFS bins |
| `operation` | Started, progress, completed, cancelled or failed |
| `dropped` | Per-source loss counters so gaps are visible in the UI |

Frame events are best effort. The device never blocks the WiFi callback on a
slow browser: it increments `dropped.frames` and keeps the newest records.
Up to three WebSocket clients are streamed at once.

### Frame event (as sent by the firmware)

```json
{"event":"frame","epoch":2,"sequence":268,"time_us":13452381,
 "data":{"sequence":268,"time_us":13452381,"rx_timestamp_us":11195756,
  "pkt_type":"data","rssi":-25,"noise":-96,"channel":6,"secondary":0,
  "rate":12,"sig_mode":0,"mcs":0,"cwb":0,"aggregation":0,"stbc":0,
  "fec_coding":0,"sgi":0,"rx_state":0,"fcs":"ok","sig_len":1504,
  "length":1500,"truncated":false,"mpdu":"<base64>"}}
```

The radio fields are `wifi_pkt_rx_ctrl_t` verbatim. `mpdu` excludes the FCS;
MPDUs longer than 1600 bytes are cut (`truncated`, `sig_len` keeps the air
length). The browser decodes frame type and addresses from `mpdu`. With
`fcs:"fail"` (`rx_state` ≠ 0) the header bytes are unreliable; on a channel
with 802.11ax/ac traffic most PPDUs fail, because the ESP32 decodes only the
legacy/HT part. RSSI and L-LTF CSI of such PPDUs are still valid.

### `frame.csi`

```json
{"event":"frame.csi","epoch":2,"time_us":13391249,
 "data":{"frame_sequence":267,"rx_timestamp_us":11134281,
  "source":"28:db:3b:82:18:fb","rssi":-82,"noise":-96,"channel":6,
  "format":"lltf_imag_real_i8","subcarriers":64,"csi":"<base64>"}}
```

L-LTF only: 64 subcarriers of `int8 imag, int8 real`, ordered 0…31, −32…−1
(null and guard tones included). `frame_sequence` is the `frame` event with
the same `rx_timestamp_us`, or `null` if that frame was filtered or dropped.
DSSS/CCK frames carry no CSI.

### `power` (source `rssi`) and `dropped`

`power` is sent every 200 ms while frames arrive: `value`/`rms` are the linear
mean of the frame RSSIs in the window (dBm), plus `peak`, `min` and `samples`.
`dropped` is sent every second with cumulative `frames`, `csi` and `ws` drop
counters and the per-second `frame_rate` and `fcs_fail_rate`.

## UDP mplane mirror

UDP port 22 exposes the same configuration and operation state for winject
automation. Standard `cmd:<u8>` request IDs are supported.

```text
analyzer_info
analyzer_mode mode=<frames|iq|sweep>
analyzer_set freq=<MHz> gain=<hardware|index>
analyzer_capture samples=<n> rate=<Hz> bits=<8|10> host=<ip> port=<port>
analyzer_sweep start=<MHz> stop=<MHz> step=<MHz> bins=<n> dwell=<n> host=<ip> port=<port>
analyzer_stop id=<u32>
```

Bulk UDP clients use the same `WJIQ` capture fragments as the WebSocket. Sweep
tiles and frame records use distinct versioned magic values so clients can
reject unknown records safely.
