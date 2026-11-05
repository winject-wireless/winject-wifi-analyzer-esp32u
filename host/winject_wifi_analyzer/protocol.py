from __future__ import annotations

from dataclasses import dataclass
import struct
import zlib


MAGIC = b"WJIQ"
VERSION = 1
FINAL = 1
HEADER = struct.Struct("<4sBBBBIIIHHI")


class ProtocolError(ValueError):
    pass


@dataclass(frozen=True)
class IQHeader:
    flags: int
    bits: int
    rate_code: int
    capture_id: int
    sample_count: int
    payload_offset: int
    payload_length: int
    payload_crc32: int

    @classmethod
    def decode(cls, datagram: bytes) -> tuple["IQHeader", bytes]:
        if len(datagram) < HEADER.size:
            raise ProtocolError("truncated IQ header")
        values = HEADER.unpack_from(datagram)
        magic, version, flags, bits, rate_code = values[:5]
        capture_id, sample_count, payload_offset = values[5:8]
        payload_length, header_length, payload_crc32 = values[8:]
        if magic != MAGIC or version != VERSION:
            raise ProtocolError("unsupported IQ datagram")
        if header_length != HEADER.size:
            raise ProtocolError("unsupported IQ header length")
        if bits not in (8, 10) or rate_code not in (0, 1, 6):
            raise ProtocolError("invalid capture format")
        payload = datagram[header_length:]
        if len(payload) != payload_length:
            raise ProtocolError("payload length mismatch")
        expected = sample_count * 2 if bits == 8 else (sample_count * 20 + 7) // 8
        if payload_offset > expected or payload_length > expected - payload_offset:
            raise ProtocolError("payload range outside capture")
        return cls(flags, bits, rate_code, capture_id, sample_count, payload_offset,
                   payload_length, payload_crc32), payload


@dataclass(frozen=True)
class Capture:
    capture_id: int
    sample_count: int
    bits: int
    rate_code: int
    payload: bytes


class CaptureAssembler:
    """Reassemble one out-of-order WJIQ capture with overlap validation."""

    def __init__(self) -> None:
        self._identity: tuple[int, int, int, int] | None = None
        self._crc = 0
        self._data = bytearray()
        self._present = bytearray()
        self._received = 0
        self._saw_final = False

    def add(self, datagram: bytes) -> Capture | None:
        header, payload = IQHeader.decode(datagram)
        identity = (header.capture_id, header.sample_count, header.bits, header.rate_code)
        total = header.sample_count * 2 if header.bits == 8 else (header.sample_count * 20 + 7) // 8
        if self._identity is None:
            self._identity = identity
            self._crc = header.payload_crc32
            self._data = bytearray(total)
            self._present = bytearray(total)
        elif identity != self._identity or header.payload_crc32 != self._crc:
            raise ProtocolError("fragment belongs to a different capture")

        start = header.payload_offset
        for index, value in enumerate(payload, start):
            if self._present[index]:
                if self._data[index] != value:
                    raise ProtocolError("conflicting duplicate fragment")
                continue
            self._data[index] = value
            self._present[index] = 1
            self._received += 1
        self._saw_final |= bool(header.flags & FINAL)
        if not self._saw_final or self._received != total:
            return None
        actual = zlib.crc32(self._data) & 0xFFFFFFFF
        if actual != self._crc:
            raise ProtocolError(f"capture CRC mismatch: {actual:08x} != {self._crc:08x}")
        capture_id, sample_count, bits, rate_code = self._identity
        return Capture(capture_id, sample_count, bits, rate_code, bytes(self._data))
