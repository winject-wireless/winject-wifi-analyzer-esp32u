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

Returns the active mode, center frequency, gain and mode-specific settings.

### `PATCH /api/v1/config`

Applies an atomic partial update. Example:

```json
{"mode":"iq","frequency_mhz":2442,"gain":{"mode":"manual","index":40}}
```

Changing mode stops the current frame stream, capture or sweep and increments
the stream epoch.

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
