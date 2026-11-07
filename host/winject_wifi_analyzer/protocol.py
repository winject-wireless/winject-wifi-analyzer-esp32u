"""WJIQ v2 sample datagrams from the ESP32U IQ server (docs/sample-protocol.md)."""
from __future__ import annotations

from dataclasses import dataclass
import struct
import zlib

import numpy as np

MAGIC = b"WJIQ"
VERSION = 2
HEADER = struct.Struct("<4sBBBBIIIHHIIIBBHHH")  # 44 bytes

FLAG_LAST_FRAGMENT = 1 << 0
FLAG_LAST_IN_STEP = 1 << 1
FLAG_LAST_STEP = 1 << 2
FLAG_AGC = 1 << 3
FLAG_LAST_CAPTURE = 1 << 4

RATE_HZ = {0: 80_000_000, 1: 40_000_000, 6: 16_000_000}
FULL_SCALE = 511.0  # 0 dBFS: |I|, |Q| = 511 circle (10-bit converter)


class ProtocolError(ValueError):
    pass


def wire_bytes(sample_count: int, bits: int) -> int:
    return sample_count * 2 if bits == 8 else (sample_count * 20 + 7) // 8


@dataclass(frozen=True)
class IQHeader:
    flags: int
    bits: int
    rate_code: int
    capture_id: int
    sample_count: int
    payload_offset: int
    payload_length: int
    fragment_crc32: int
    frequency_khz: int
    timestamp_us: int
    gain_index: int
    bandwidth_mhz: int
    job_id: int
    step: int
    steps: int

    @classmethod
    def decode(cls, datagram: bytes) -> tuple["IQHeader", bytes]:
        if len(datagram) < HEADER.size:
            raise ProtocolError("truncated IQ header")
        (magic, version, flags, bits, rate_code, capture_id, sample_count,
         payload_offset, payload_length, header_length, crc, freq_khz, ts,
         gain, bw, job, step, steps) = HEADER.unpack_from(datagram)
        if magic != MAGIC or version != VERSION:
            raise ProtocolError("unsupported IQ datagram")
        if header_length < HEADER.size or header_length > len(datagram):
            raise ProtocolError("bad IQ header length")
        if bits not in (8, 10) or rate_code not in RATE_HZ:
            raise ProtocolError("invalid capture format")
        payload = datagram[header_length:]
        if len(payload) != payload_length:
            raise ProtocolError("payload length mismatch")
        total = wire_bytes(sample_count, bits)
        if payload_offset + payload_length > total:
            raise ProtocolError("payload range outside capture")
        if zlib.crc32(payload) & 0xFFFFFFFF != crc:
            raise ProtocolError("fragment CRC mismatch")
        return cls(flags, bits, rate_code, capture_id, sample_count, payload_offset,
                   payload_length, crc, freq_khz, ts, gain, bw, job, step,
                   steps), payload


@dataclass(frozen=True)
class Capture:
    capture_id: int
    job_id: int
    sample_count: int
    bits: int
    rate_code: int
    frequency_hz: int
    timestamp_us: int
    gain_index: int
    agc: bool
    bandwidth_mhz: int
    step: int
    steps: int
    flags: int
    payload: bytes

    @property
    def sample_rate_hz(self) -> int:
        return RATE_HZ[self.rate_code]

    def samples(self) -> np.ndarray:
        """Complex64 IQ in conventional sign, |x| = 1 at 0 dBFS."""
        if self.bits == 8:
            raw = np.frombuffer(self.payload, dtype=np.int8).astype(np.float32) * 4.0
            i, q_raw = raw[0::2], raw[1::2]
        else:
            b = np.frombuffer(self.payload, dtype=np.uint8)
            pad = (-len(b)) % 5
            if pad:
                b = np.concatenate([b, np.zeros(pad, np.uint8)])
            g = b.reshape(-1, 5).astype(np.uint64)
            v = g[:, 0] | (g[:, 1] << 8) | (g[:, 2] << 16) | (g[:, 3] << 24) | (g[:, 4] << 32)
            words = np.empty(len(v) * 2, np.int64)
            words[0::2] = (v & 0xFFFFF).astype(np.int64)
            words[1::2] = ((v >> 20) & 0xFFFFF).astype(np.int64)
            words = words[: self.sample_count]

            def sext10(x: np.ndarray) -> np.ndarray:
                x = x & 0x3FF
                return np.where(x & 0x200, x - 1024, x).astype(np.float32)

            i, q_raw = sext10(words), sext10(words >> 10)
        return ((i - 1j * q_raw) / FULL_SCALE).astype(np.complex64)


class _Partial:
    __slots__ = ("header", "data", "offsets", "received", "total", "saw_last")

    def __init__(self, header: IQHeader) -> None:
        self.header = header
        self.total = wire_bytes(header.sample_count, header.bits)
        self.data = bytearray(self.total)
        self.offsets: set[int] = set()
        self.received = 0
        self.saw_last = False


class StreamAssembler:
    """Reassembles interleaved WJIQ v2 captures; counts losses and errors."""

    def __init__(self, window: int = 8) -> None:
        self._open: dict[int, _Partial] = {}
        self._window = window
        self.completed = 0
        self.lost = 0
        self.errors = 0

    def add(self, datagram: bytes) -> Capture | None:
        try:
            header, payload = IQHeader.decode(datagram)
        except ProtocolError:
            self.errors += 1
            return None
        part = self._open.get(header.capture_id)
        if part is None:
            part = _Partial(header)
            self._open[header.capture_id] = part
            # Older captures still open after a newer one started are lost.
            while len(self._open) > self._window:
                oldest = min(self._open)
                del self._open[oldest]
                self.lost += 1
        if header.payload_offset not in part.offsets:
            part.offsets.add(header.payload_offset)
            part.data[header.payload_offset:header.payload_offset + len(payload)] = payload
            part.received += len(payload)
        if header.flags & FLAG_LAST_FRAGMENT:
            part.saw_last = True
            part.header = header  # carries the capture's job flags
        if part.received != part.total:
            return None
        del self._open[header.capture_id]
        self.completed += 1
        h = part.header
        return Capture(h.capture_id, h.job_id, h.sample_count, h.bits, h.rate_code,
                       h.frequency_khz * 1000, h.timestamp_us, h.gain_index,
                       bool(h.flags & FLAG_AGC), h.bandwidth_mhz, h.step, h.steps,
                       h.flags, bytes(part.data))
