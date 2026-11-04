"""Client for the ESP-SDR serial protocol (firmware/esp-sdr/docs/protocol.md).

One text command per line, exactly one reply line each; captures are followed by
a CRC32-checked binary payload of packed (I, -Q) samples.
"""

from __future__ import annotations

import json
import random
import time
import zlib
from dataclasses import dataclass

import numpy as np

# Rate codes from protocol.md, MS/s.
RATE_MSPS = {0: 80.0, 1: 40.0, 2: 20.0, 3: 10.0, 4: 8.0, 5: 4.0, 6: 16.0,
             7: 32.0, 8: 10.667, 9: 6.4}

# Full-scale magnitude per capture format (signed integers).
FULL_SCALE = {16: 127.0, 20: 511.0}


class SdrError(RuntimeError):
    """The firmware answered ERR, or the reply was malformed."""


def rate_code(msps: float) -> int:
    for code, value in RATE_MSPS.items():
        if abs(value - msps) < 0.01:
            return code
    raise ValueError(f"unsupported sample rate {msps} MS/s; choose from {sorted(RATE_MSPS.values())}")


def decode_cap16(payload: bytes, samples: int) -> np.ndarray:
    """Signed 8-bit I, then signed 8-bit -Q, per sample."""
    raw = np.frombuffer(payload, dtype=np.int8, count=2 * samples).astype(np.float32)
    return raw[0::2] - 1j * raw[1::2]


def decode_cap20(payload: bytes, samples: int) -> np.ndarray:
    """20 bits per sample, packed LSB first: I in bits 0-9, -Q in bits 10-19."""
    bits = np.unpackbits(np.frombuffer(payload, dtype=np.uint8), bitorder="little")
    bits = bits[: samples * 20].reshape(samples, 2, 10).astype(np.int32)
    weights = 1 << np.arange(10, dtype=np.int32)
    vals = (bits * weights).sum(axis=2)
    vals = np.where(vals >= 512, vals - 1024, vals).astype(np.float32)
    return vals[:, 0] - 1j * vals[:, 1]


def payload_len(samples: int, bits: int) -> int:
    """Bytes after the DATA line; `bits` is per complex sample (16 or 20)."""
    return -(-samples * bits // 8)


def parse_pwr(reply: str, tag: str = "PWR") -> dict:
    """Parse 'PWR caps=.. bursts=.. med=.. ... gain=MANUAL_40' (or a PWRC reply)."""
    parts = reply.split()
    if not parts or parts[0] != tag:
        raise SdrError(f"unexpected PWR reply: {reply}")
    out: dict = {}
    for kv in parts[1:]:
        key, _, value = kv.partition("=")
        if key == "gain":
            out[key] = value
        else:
            out[key] = float(value)  # float("nan") parses the firmware's "nan"
    return out


@dataclass
class Capture:
    iq: np.ndarray          # complex64, standard (I, Q)
    rate_msps: float
    bits: int               # 16 or 20 (total bits per complex sample)
    capture_us: int         # firmware capture time stamp from the DATA line

    @property
    def full_scale(self) -> float:
        return FULL_SCALE[self.bits]


class EspSdr:
    """Talks to ESP-SDR over a serial port (921600 baud: this repo's firmware build)."""

    def __init__(self, port: str, baud: int = 921_600, timeout: float = 2.0):
        import serial  # pyserial; imported here so decoding works without it

        # Do not toggle DTR/RTS: on UART bridges they are wired to EN/BOOT.
        self.ser = serial.Serial()
        self.ser.port = port
        self.ser.baudrate = baud
        self.ser.timeout = timeout
        self.ser.dtr = False
        self.ser.rts = False
        self.ser.open()
        self.sync()

    def close(self) -> None:
        try:
            self.command("RELEASE")
        except (SdrError, OSError):
            pass
        self.ser.close()

    def __enter__(self) -> "EspSdr":
        return self

    def __exit__(self, *exc) -> None:
        self.close()

    # -- line protocol -----------------------------------------------------

    def _readline(self) -> str:
        line = self.ser.readline()
        if not line:
            raise SdrError("timeout waiting for reply")
        return line.decode("ascii", errors="replace").strip()

    def sync(self, attempts: int = 5) -> None:
        """Discard boot output and stale bytes until the nonce is echoed."""
        for _ in range(attempts):
            nonce = str(random.getrandbits(32))  # firmware parses a decimal u64
            self.ser.reset_input_buffer()
            self.ser.write(f"SYNC {nonce}\n".encode())
            deadline = time.monotonic() + 3.0
            while time.monotonic() < deadline:
                line = self.ser.readline().decode("ascii", errors="replace").strip()
                if line == f"SYNC {nonce}":
                    return
            time.sleep(0.5)  # board may still be booting after the port opened
        raise SdrError("no SYNC echo; is ESP-SDR running at this baud rate?")

    def command(self, line: str) -> str:
        self.ser.write((line + "\n").encode())
        reply = self._readline()
        if reply.startswith("ERR"):
            raise SdrError(f"{line!r}: {reply}")
        return reply

    # -- session / settings --------------------------------------------------

    def info(self) -> str:
        return self.command("INFO")

    def limits(self) -> dict:
        reply = self.command("LIMITS?")
        if not reply.startswith("LIMITS "):
            raise SdrError(f"unexpected LIMITS reply: {reply}")
        return json.loads(reply[len("LIMITS "):])

    def set_freq(self, mhz: int) -> str:
        return self.command(f"FREQ {int(mhz)}")

    def set_gain_manual(self, index: int) -> str:
        return self.command(f"GAIN MANUAL {int(index)}")

    def set_gain_hardware(self) -> str:
        return self.command("GAIN HARDWARE")

    def gain(self) -> str:
        return self.command("GAIN?")

    def set_bandwidth(self, mhz: int) -> str:
        return self.command(f"BANDWIDTH {int(mhz)}")

    # -- on-chip power measurement (firmware/power_meter) ----------------------

    def pwr(self, captures: int = 20, rate_msps: float = 80.0, threshold_db: int = 10,
            min_burst_us: int = 20) -> dict:
        """Run PWR on the ESP32; returns its key=value reply as a dict.

        Numeric fields are floats (NaN when there were no bursts); `gain` is a
        string. The reply takes captures x (snapshot + processing) time.
        """
        code = rate_code(rate_msps)
        old = self.ser.timeout
        self.ser.timeout = max(old, 2.0 + 0.1 * captures)
        try:
            reply = self.command(f"PWR {int(captures)} {code} {int(threshold_db)} {int(min_burst_us)}")
        finally:
            self.ser.timeout = old
        return parse_pwr(reply)

    def pwrc(self, captures: int = 20, rate_msps: float = 40.0) -> dict:
        """PWRC: mean power of whole snapshots, for a continuous (CW) signal."""
        code = rate_code(rate_msps)
        old = self.ser.timeout
        self.ser.timeout = max(old, 2.0 + 0.1 * captures)
        try:
            reply = self.command(f"PWRC {int(captures)} {code}")
        finally:
            self.ser.timeout = old
        return parse_pwr(reply, "PWRC")

    # -- captures -------------------------------------------------------------

    def capture(self, samples: int, rate_msps: float = 20.0, bits: int = 20) -> Capture:
        if bits not in FULL_SCALE:
            raise ValueError("bits must be 16 or 20")
        code = rate_code(rate_msps)
        self.ser.write(f"CAP{bits} {int(samples)} {code}\n".encode())
        header = self._readline()
        if header.startswith("ERR"):
            raise SdrError(f"CAP{bits}: {header}")
        parts = header.split()
        if len(parts) != 4 or parts[0] != "DATA":
            raise SdrError(f"unexpected capture reply: {header}")
        n, crc_hex, cap_us = int(parts[1]), int(parts[2], 16), int(parts[3])
        want = payload_len(n, bits)
        payload = self.ser.read(want)
        if len(payload) != want:
            raise SdrError(f"short payload: {len(payload)} of {want} bytes")
        if zlib.crc32(payload) & 0xFFFFFFFF != crc_hex:
            raise SdrError("payload CRC mismatch")
        iq = decode_cap20(payload, n) if bits == 20 else decode_cap16(payload, n)
        return Capture(iq=iq.astype(np.complex64), rate_msps=rate_msps, bits=bits,
                       capture_us=cap_us)
