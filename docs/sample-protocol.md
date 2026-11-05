# WJIQ sample fragments

Raw IQ captures use application framing over UDP or binary WebSocket messages.
Every fragment starts with this 28-byte little-endian header:

```c
struct winject_iq_header {
    char     magic[4];       // "WJIQ"
    uint8_t  version;        // 1
    uint8_t  flags;          // bit 0: final fragment
    uint8_t  bits;           // 8 or 10 per I/Q component
    uint8_t  rate_code;      // 0=80, 1=40, 6=16 MS/s
    uint32_t capture_id;
    uint32_t sample_count;
    uint32_t payload_offset;
    uint16_t payload_length;
    uint16_t header_length;  // 28
    uint32_t payload_crc32;  // complete packed capture
};
```

Fragments may arrive out of order or be duplicated. A client writes by offset,
rejects conflicting overlaps, and completes only when every byte is present
and the complete payload CRC32 matches.

IQ8 is signed `int8 I, int8 -Q`. IQ10 is ESP-SDR CAP20 packing: two 20-bit
`(I, -Q)` samples occupy five bytes, least-significant bit first. Negate Q for
conventional complex samples.
