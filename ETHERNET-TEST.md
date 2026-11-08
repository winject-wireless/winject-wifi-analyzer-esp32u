# ESP32U Ethernet test image

Historical test image based on analyzer commit 275ec73. It retained the analyzer and
raw Ethernet stack, and adds UDP benchmark control on port 5500 and data on
5501. LAN8720 uses MDC 23, MDIO 18, PHY power 16 and REF_CLK output 17.

Build: `pio run -e lan-module`. The diagnostic ELF SHA-256 at that time was:
`1e187de42da0d603ea1708a05ae0d18d4e03dd268c3e18393bb3d054fafee8c0`.

From a host on the same Ethernet segment, use the DHCP address or the static
fallback (192.168.32.1; put a host address such as 192.168.32.2/24 on the
host interface if needed):

```
python3 tools/ether_bench.py <address> --direction rx --rate 30000 --seconds 5
python3 tools/ether_bench.py <address> --direction tx --rate 30000 --seconds 5
python3 tools/ether_bench.py <address> --direction both --rate 30000 --seconds 5
```

Directions are relative to ESP32. Rate is target kbit/s *per direction*;
reported offer and goodput count 1400-byte UDP payloads and use the configured
interval. Host-to-ESP32 loss is sent count minus ESP32 received count; it
assumes no duplicates or unrelated ETST traffic. ESP32-to-host loss counts
unique sequence numbers relative to sends accepted by the driver; send errors
are separate. Device receive queue drops are also shown. Control datagrams
are unencrypted and intended for a trusted local test network. The tool is
not iperf-compatible. At high rates host CPU, socket buffer, scheduling and
network path may limit results. The benchmark has not been validated end to end
because the board is not reachable on this host's Ethernet segment.

The previous application was read from flash and saved as
`../esp32u-original-app.bin` (720896 bytes, includes flash padding; image hash
and checksum verified). Restore with the same esptool version using
`--chip esp32 --port /dev/ttyUSB1 --baud 115200 write_flash 0x20000 ../esp32u-original-app.bin`.
That test image was verified after write. These historical image and restore instructions do not identify the currently running firmware. Serial log is
`../ethernet-test-boot.log`.

Diagnostic image logs driver RX, broadcast, probe, ARP reply and TX counters every five seconds. On Nov 7 the ESP32 received 100/100 host UDP broadcast probes and queued three ARP replies; none of those replies appeared on the host. The serial capture is ../ethernet-rx-diagnostic-boot.log.
