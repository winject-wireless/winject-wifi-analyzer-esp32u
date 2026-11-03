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
| `host/` | Host tool (Python) |
| `tests/` | Host tests |

## Firmware

ESP-SDR pins its own ESP-IDF commit per target in
`firmware/esp-sdr/firmware-targets.json` (ESP32: `25fe69f`). Build and flash
it as its README describes, for target `esp32` (UART0, 2 Mbaud).

## Status

Work in progress.

## License

`firmware/esp-sdr` is GPLv3 (see its `LICENSE`). It is used unmodified as a
separate program; the host tool talks to it over its serial protocol.
