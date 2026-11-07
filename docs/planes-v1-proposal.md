# ESP32U sample and management planes: initial proposal

**Status: design proposal, not implemented.** The firmware currently uses WJIQ
v2 unicast and `iq_*` commands as described in [sample-protocol.md](sample-protocol.md)
and [radio-server.md](radio-server.md). This document specifies a proposed
replacement for review. The test firmware currently on the board does not
implement it.

## Model

The ESP32 captures a finite burst of complex samples into its hardware dump
buffer, then transmits it. It cannot guarantee continuity between bursts.
A sampling sequence is one such burst. The sender repeats captures as quickly
as it can while streaming, subject to capture and network capacity; a sample
rate describes spacing *within* a burst, not the average delivered rate.
No retransmission is required.

## S-plane

UDP/IPv4 multicast, default group `239.255.53.1`, destination port `5300`,
TTL 1. Group and port are device configuration and are reported by m-plane.
The sender maps the multicast IP to its Ethernet multicast MAC (not ARP). A
listener joins the group on its intended interface and identifies a device by
source IP. Network switches with IGMP snooping must have group membership;
multicast forwarding across subnets is outside v1.

All multibyte integers below are **little-endian**, with no padding. Each UDP
payload begins with `msg_type`. UDP datagrams are at most 1417 bytes, so IPv4
plus UDP stays below a 1500-byte IP MTU without fragmentation. Unknown types
are ignored. Port 5300 and the m-plane `splane_version=1` select this format;
it is not compatible with WJIQ v2 despite reusing the port.

### Samples: type 1

| Offset | Type | Field | Meaning |
| --- | --- | --- | --- |
| 0 | u8 | `msg_type` | 1 |
| 1 | u32 | `sampling_sequence_number` | Capture/burst identifier |
| 5 | u32 | `sample_index` | Index of the first complex sample in this datagram, relative to the burst |
| 9 | u64 | `sample0_time_ns` | Time of sample index **0 of the burst**, same in every fragment; 0 means unknown |
| 17 | bytes | `complex_samples` | 1–700 complex samples, 2 bytes each |

IQ8 format is interleaved signed two's-complement bytes `I, Qraw`, with the
upper 8 bits of each 10-bit converter field; the conventional complex value is
`I - j*Qraw`. The payload length after byte 17 must be even and nonzero. It
contains `payload_length / 2` complex samples; `sample_index + count` must
not exceed the configured sample depth. There is no padding. A fragment may
arrive out of order, be duplicated, or be lost. Receivers place samples by
index and use configured depth to detect incomplete bursts; an entirely lost
burst can be detected from later sequence numbers but a missing final burst at
stream end cannot be inferred from this packet format alone.

A successful capture receives the next u32 sequence number, incremented once
per captured burst, including a burst whose send later fails; it wraps modulo
2^32. A failed capture increments `capture_errors` but does not consume a
sequence number. Start chooses a new initial sequence number; receivers treat
start, stop, reboot or source address change as a discontinuity and must not
join fragments across them. The sender sends each burst's fragments in index
order and does not interleave different bursts; receivers still tolerate
network reordering. UDP checksums are enabled; there is no application CRC in
v1.

`sample0_time_ns` is reserved for a monotonic device clock in nanoseconds
since boot, with the actual sample-zero reference and accuracy specified by
`time_info`. **The first implementation must send 0** until the capture
instant and its error are measured. The current `esp_timer` reading before
buffer preparation is not sample-zero time. Nanosecond units do not imply
nanosecond precision. A reboot resets this clock.

### Frame events

Frame-trigger events are not part of this initial protocol. The OPi5 detects
802.11-like preamble candidates directly from IQ; the ESP32U does not register
a promiscuous receive callback or transmit frame-event datagrams. On the
ESP32U, a 15-second test without promiscuous mode produced valid multicast IQ
bursts and Wi-Fi-like preamble candidates on the OPi5; capture and send
errors stayed at zero.

### Receiver setup and limitations

1. Query m-plane for `splane_version`, group, port, sample format, depth,
   rate, frequency, state and configuration revision. Join the group before
   starting a stream.
2. While streaming, settings are immutable. Reassemble by source address,
   sequence number and index. On gaps or overlap, mark the capture incomplete;
   do not fill missing IQ with valid-looking zeros.
3. On a stop/restart or configuration change, discard incomplete bursts and
   query configuration again. Periodically query status to detect a reboot or
   unexpected configuration change.

The proposed compact sample header carries neither a stream/session ID nor a
configuration revision. Therefore packets alone cannot unambiguously link a
late-joining receiver to a specific configuration, or distinguish all delayed
old packets from new packets after restart/reboot. An m-plane query plus
source IP and sequence is only a best-effort association. If those guarantees
are required, add a session/configuration identifier to every sample packet
before implementing the format; this is an open design decision, not a
property supplied by UDP multicast.

## M-plane

The separate [m-plane v1 specification](mplane-v1.md) defines the control
protocol. The m-plane specification is complete; the s-plane above remains a
proposal and neither replacement is implemented yet.
