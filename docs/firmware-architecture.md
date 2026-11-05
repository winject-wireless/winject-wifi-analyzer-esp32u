# Firmware architecture

The implementation should reuse two existing, proven pieces without merging
their hot paths blindly:

- `winject-radio-esp32`: LAN8720 clock-out wiring, DHCP/static fallback,
  mplane request IDs, HTTP update and non-blocking promiscuous frame queues.
- `firmware/esp-sdr`: ESP32 RF tuning, bounded raw-I/Q capture, gain control and
  FFT/spectrum routines.

## Tasks and ownership

```text
CPU0  WiFi driver / promiscuous callback ──▶ bounded frame queue ──▶ event encoder
      ESP-SDR snapshot capture (IQ and sweep modes; exclusive with frame mode)

CPU1  EMAC RX/TX (LAN8720) ──▶ lwIP ──▶ HTTP + WebSocket + mplane
                              └───────▶ WJIQ UDP sender
```

The WiFi callback copies the complete MPDU and RX metadata into a fixed pool,
then returns. It never formats JSON or waits for a browser. CSI is copied into
a second bounded record keyed to the frame sequence when the IDF callback
provides a usable association. Queue exhaustion increments counters delivered
as `dropped` events.

IQ capture owns the RF backend. Switching from `frames` to `iq` or `sweep`
stops promiscuous/CSI delivery before ESP-SDR reconfigures PBUS and dump SRAM.
Switching back fully restores the WiFi driver and increments the stream epoch.

## Board target

Use only the original winject ESP32U LAN module environment:

```text
PHY          LAN8720, address 1
MDC / MDIO   GPIO23 / GPIO18
PHY power    GPIO16, active high
RMII clock   ESP32 APLL 50 MHz output on GPIO17
```

The target must configure `EMAC_CLK_OUT` on GPIO17. The WT32-ETH01 external
clock input configuration is a distinct board and must not become the default.

## Memory policy

The original ESP32 has little spare internal SRAM and ESP-SDR reserves a 64 KiB
RF dump aperture. Therefore:

- frame and CSI queues are fixed-size pools with explicit drop counters;
- web assets live compressed in flash and are streamed without a heap copy;
- raw IQ is packetized directly from the capture buffer after in-place packing;
- only one browser raw-capture consumer is supported initially;
- spectrum sweeps reduce each snapshot to bins before retaining the next tile.

## Delivery order

1. LAN8720 clock-out bring-up, DHCP/fallback address and mplane/HTTP liveness.
2. Frame mode: MPDU/RSSI metadata, fixed pool, WebSocket events and drop tests.
3. CSI records, with association and `first_word_invalid` represented honestly.
4. IQ mode: ESP-SDR capture plus WJIQ WebSocket/UDP delivery and CRC tests.
5. IQ power detector and sweep scheduler with cancellation between snapshots.
6. Hardware validation: queue loss, Ethernet throughput, mode restoration,
   frequency/gain sanity, tone linearity and calibrated power offsets.
