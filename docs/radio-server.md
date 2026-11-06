# ESP32U IQ sample server

The ESP32U is a pure radio/sample server. It captures raw baseband I/Q with
ESP-SDR's RF dump and streams it over Ethernet; all processing (spectrum,
power, tone, sweep FFTs) and the web app run on a host
(`python3 -m winject_wifi_analyzer --host <esp32u> serve`).

```
ESP32 RF dump ──► IQ task ──► WJIQ v2 UDP (raw Ethernet) ──► host server ──► browser (WebSocket)
                    ▲                                         │
                    └──────── mplane `iq_*` on UDP 22 ◄────────┘
```

## Firmware

| Part | Where | Notes |
| --- | --- | --- |
| RF dump, tuning, gain, LPF | `components/analyzer_iq` | GPLv3, adapted from ESP-SDR `targets/esp32`; needs the same `libphy.a`/`librtc.a` as ESP-SDR's pinned IDF (IDF 6.0.1's are byte-identical) |
| IQ server | `src/winject-esp32/iq/iq_server.cpp` | `iq_*` commands, capture/sweep jobs, WJIQ v2 packing |
| Raw IPv4 stack | `src/winject-esp32/net/` | No lwIP or esp_netif: ARP (reply, cache, resolve), IPv4, UDP, ICMP echo, DHCP client with static fallback; `net_proto` is host-tested |
| mplane | `src/winject-esp32/net/mplane_udp.cpp` | UDP 22, same parser (`mplane/`) and `cmd:<u8>` correlation as winject |
| WiFi | `main.cpp` | ESP-SDR minimal init (mode NULL + promiscuous) only to power and calibrate the PHY |

There is no HTTP server and no OTA: flash over serial
(`pio run -e lan-module -t upload --upload-port /dev/ttyUSB1`). Opening the
ESP32U's CP2102 port power-resets the board.

Memory: static DRAM ~42 KiB; the RF dump reserves 64 KiB at `0x3ffe8000`.
EMAC DMA rings are 10 RX / 20 TX (a 16380-sample IQ8 capture is 24 datagrams).

### Network

- DHCP (hostname `winject-iq-esp32u`), falling back after `timeout` seconds to
  the configured static address (default `192.168.32.1/24`) while DHCP keeps
  retrying; lease renewal at T1 (broadcast). `network ...` / `save` / `load`
  work as in winject.
- Frames are accepted for our MAC or broadcast, IPv4 or ARP only; IP
  fragments are dropped.
- Measured: 16380 × IQ8 @ 16 MS/s back to back = 177 captures/s, 48 Mbit/s;
  IQ10 = 112/s; 4096 × IQ8 @ 40 MS/s = 412/s; 0 lost.

## mplane `iq_*` commands

Replies follow docs/mplane.md (`OK:<id> …`, `NOK:<id> <code>` with a
`cmd:<id>` prefix). The winject radio/test commands (`radio_tx`, `tx_info`,
`test_*` …) answer `NOK ENODEV`.

| Command | Reply |
| --- | --- |
| `iq_info` | two lines: `proto=2 rates=… bits=8,10 max_n=16380 freq_min=100 freq_max=6000 gain_max=<n> freq=<MHz> gain=agc\|<n> bw=<MHz>`, then `job=<id> kind=idle\|capture\|sweep dst=<ip:port> captures=… datagrams=… tx_errors=… capture_errors=…` |
| `iq_set [freq=<100..6000 MHz>] [gain=agc\|<0..gain_max>] [bw=<0\|12..67 MHz>]` | `OK iq_set freq=… gain=… bw=…`; retunes now (fresh RX DC calibration). `bw` 0 = PHY default low-pass |
| `iq_capture [n=256..16380] [rate=80\|40\|16 (MS/s or Hz)] [bits=8\|10] [count=<n>, 0 = until iq_stop] [interval_ms=<0..60000>] [dst=<ip>[:port]]` | `OK iq_capture id=<job> dst=… n=… bits=… rate=…`; defaults n=16380, 16 MS/s, IQ8, count=1, dst = requester:5300 |
| `iq_sweep start=<MHz> stop=<MHz> step=<1..80 MHz> [n=…] [rate=…] [bits=…] [dwell=<1..256>] [passes=<n>, 0 = until iq_stop] [dst=…]` | `OK iq_sweep id=<job> …`; per step: tune, `dwell` captures tagged with the step; tunes back to `freq` when done |
| `iq_stop` | `OK iq_stop`; ends the running job between captures |

A new `iq_capture`/`iq_sweep` replaces the running job. The sweep uses the
current gain and low-pass; the host server sets a fixed gain and a low-pass of
~0.8 × fs for sweeps and restores them afterwards.

## Host server

`host/winject_wifi_analyzer/server.py` (system `websockets` 10.4 + numpy):

- serves `web/` on `--http-port` (default 8080), WebSocket at `/ws`;
- starts a continuous capture (`count=0`) to itself and renders the newest
  capture at ~12 Hz: Welch spectrum (Hann, 50 % overlap, exponential average
  or peak hold), mean/peak/DC power, clipping, strongest tone, envelope
  (peak |x| per bucket) and 512 constellation points;
- `snapshot` freezes the latest capture (4096 points) and serves it as
  `/capture/snapshot.cf32` (complex64 LE, |x| = 1 at 0 dBFS) and `.json`;
- sweeps: averages (or max-holds) the PSDs of each step's dwell captures and
  keeps the central `step` of the spectrum as one tile.

Browser messages (JSON): `{"op":"config", freq_mhz, rate_hz, gain, bw_mhz,
n, bits, nfft, average, detector}`, `{"op":"live","on":bool}`,
`{"op":"snapshot"}`, `{"op":"sweep", start_mhz, stop_mhz, step_mhz, rate_hz,
fft, dwell, detector, repeat}`, `{"op":"stop_sweep"}`. Events: `status` (1 Hz),
`live`, `snapshot`, `sweep.start`, `sweep.tile`, `sweep.done`, `error`.

0 dBFS is a sample on the 511 circle of the 10-bit converter. Readings are
relative; with the hardware AGC the gain moves with the signal, so fix the
gain for linear power measurements. Absolute dBm needs a calibration per gain
index and frequency.
