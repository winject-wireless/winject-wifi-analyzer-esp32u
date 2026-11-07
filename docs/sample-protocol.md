# WJIQ v2 sample datagrams

For the proposed replacement, see [sample and management planes v1](planes-v1-proposal.md). This page documents the current implementation.

Every capture leaves the ESP32U as UDP datagrams (source port 5300) of a
44-byte little-endian header plus up to 1400 payload bytes:

```c
struct wjiq_v2_header {
    char     magic[4];        // "WJIQ"
    uint8_t  version;         // 2
    uint8_t  flags;           // see below
    uint8_t  bits;            // 8 (IQ8) or 10 (IQ10)
    uint8_t  rate_code;       // 0 = 80, 1 = 40, 6 = 16 MS/s
    uint32_t capture_id;      // increments per capture (all jobs)
    uint32_t sample_count;
    uint32_t payload_offset;  // byte offset in the packed capture
    uint16_t payload_length;
    uint16_t header_length;   // 44; skip unknown trailing header bytes
    uint32_t fragment_crc32;  // CRC-32 (zlib) of this datagram's payload
    uint32_t frequency_khz;   // receive LO at capture time
    uint32_t timestamp_us;    // capture start, ESP32 esp_timer low 32 bits
    uint8_t  gain_index;      // gain recorded in the first sample
    uint8_t  bandwidth_mhz;   // analog low-pass setting (0 = PHY default)
    uint16_t job_id;          // iq_capture / iq_sweep id
    uint16_t step;            // sweep step (0 for captures)
    uint16_t steps;           // sweep steps (1 for captures)
};
```

Flags: bit 0 last fragment of the capture, bit 1 last capture of a sweep
step, bit 2 last step of a sweep pass, bit 3 hardware AGC, bit 4 last capture
of the job (finite `count`/`passes` only). Step/job flags are set on every
fragment of that capture.

A capture is complete when all `wire_bytes(sample_count, bits)` bytes have
arrived (fragments may be reordered; fragments of different captures do not
interleave on the wire but can be lost). There is no retransmission: a
missing fragment loses the capture (`StreamAssembler.lost`).

Payload packing (ESP-SDR CAP16 / CAP20, raw Q field):

- IQ8: per sample `int8 I, int8 Qraw` = the upper 8 bits of the 10-bit fields;
  2 bytes/sample.
- IQ10: two 20-bit `(I | Qraw << 10)` words in five bytes, LSB first; an odd
  tail takes three bytes.

Conventional complex samples are `I - j·Qraw`. `protocol.Capture.samples()`
returns them as complex64 scaled so |x| = 1 is 0 dBFS (I, Q in 10-bit units
divided by 511).
