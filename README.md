# esp32-power-meter

A 2.4 GHz burst power meter built from an ESP32 (ESP32-WROOM-32U, u.FL) and a
host tool. The ESP32 runs ESPARGOS's [ESP-SDR](https://github.com/ESPARGOS/esp-sdr)
firmware, which reads raw baseband I/Q from the radio over an undocumented
debug path. The host tool holds the receiver at a fixed gain, captures I/Q
bursts and reports the power of the frames it sees.

It is meant for comparing winject transmitters (ESP32-C6, ESP32, RTL8811AU)
through a fixed attenuator on the bench.

## What it measures

- **Relative power, linear in dB.** With the gain fixed (`GAIN MANUAL`), the
  I/Q power follows the input power 1:1 until the ADC clips. Differences
  between transmitters and between power settings are therefore real dB.
- **Absolute power only after calibration.** ESP-SDR gain indices are PHY
  table entries, not dB, and no full-scale level is documented. One reading of
  a transmitter with a known output (power meter or calibrated source) gives a
  dBFS → dBm offset per gain index and frequency.
- **Spectrum.** Where the transmitter's power lands: in the 20 MHz channel,
  on the carrier, in an image or in spurs.

## Layout

| Path | Contents |
| --- | --- |
| `firmware/esp-sdr` | ESP-SDR, git submodule, unmodified (GPLv3) |
| `firmware/power_meter/pwr_command.h` | On-chip `PWR` measurement command (GPLv3) |
| `firmware/patches/` | Hooks `PWR` into ESP-SDR's ESP32 receiver |
| `firmware/build.sh` | Copies the submodule, applies the patches, builds/flashes |
| `host/esp32_power_meter/` | Host tool (Python: pyserial, numpy) |
| `tests/` | Host tests, including a gcc test of `pwr_command.h` |

## Firmware

```sh
git submodule update --init --recursive
. ~/esp/esp-idf-esp-sdr/export.sh      # ESP-SDR's pinned IDF (firmware-targets.json, ESP32: 25fe69f)
firmware/build.sh flash /dev/ttyUSB0
```

IDF v5.5.5 builds but the firmware hangs in `app_main` (PHY internals differ);
use the pinned commit. The UART runs at 921600 baud: the ESP32-U board's
original CP2102 cannot do ESP-SDR's default 2 Mbaud.

The ESP32 measures on-chip; only one summary line crosses the serial link:

```text
PWR <captures> <rate> <threshold_db> <min_burst_us>
PWR caps=20 bursts=37 med=-23.10 p10=-23.40 p90=-22.85 min=... max=... peak=...
    noise=-61.20 clip_ppm=0 len_us=121.50 gain=MANUAL_40
```

Rate codes: 0 = 80, 1 = 40, 6 = 16 MS/s. Each capture is a 16,380-sample
snapshot; bursts are runs whose 1 µs average power is `threshold_db` above the
capture's noise floor (20th percentile) and at least `min_burst_us` long.
Burst power is the mean over the burst with 10 % trimmed from each end.

## Host tool

```sh
cd host
python3 -m esp32_power_meter info
python3 -m esp32_power_meter gain-scan --freq 2412            # pick a gain: 1:1, no clipping
python3 -m esp32_power_meter measure --freq 2412 --gain 40 --captures 50
python3 -m esp32_power_meter calibrate --freq 2412 --gain 40 --known-dbm -30
python3 -m esp32_power_meter measure --freq 2412 --gain 40 --offset-db 12.3
python3 -m esp32_power_meter spectrum --freq 2412 --gain 40   # debug: raw I/Q to the host
```

Tests: `python3 -m unittest discover -s tests` and
`cc -O2 -o /tmp/pwr_host_test tests/pwr_host_test.c -lm && /tmp/pwr_host_test`.

## Status

Work in progress.

## License

`firmware/esp-sdr` is GPLv3 (see its `LICENSE`). The firmware this repo builds
(ESP-SDR plus `firmware/power_meter` and `firmware/patches`) is therefore GPLv3
as a whole; `pwr_command.h` carries that notice. The host tool is a separate
program that talks to the firmware over its serial protocol.
