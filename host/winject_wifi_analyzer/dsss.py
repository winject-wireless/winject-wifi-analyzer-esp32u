"""Optional FCS-gated 802.11b decoder using the MIT-licensed crafter tool.

This adapter invokes the separately built Rust ``radio_receive`` binary; it
is not a C++ decoder and does not bundle or download it. Each IQ capture is
replayed in its own process: capture gaps never become fabricated samples.
Use this in a bounded background worker, not on the network event loop.
"""
from __future__ import annotations

import json
import os
from pathlib import Path
import subprocess
import tempfile

import numpy as np

from .mpdu_decoder import parse_mpdu

CRAFTER_REVISION = '26347a513bfbf0f5f23a330a83c87ce1ba4667fc'
SAMPLE_RATE = 20_000_000
# 40 MHz -> 20 MHz: real, unity-gain 81-tap low-pass at 9.5 MHz.
# Centered convolution compensates group delay; edge samples use zero padding.
_k = np.arange(-40, 41, dtype=np.float64)
_h = 2 * (9.5 / 40) * np.sinc(2 * (9.5 / 40) * _k) * np.kaiser(81, 8.0)
_DOWNSAMPLE_FILTER = (_h / _h.sum()).astype(np.float32)


class DsssUnavailable(RuntimeError):
    """Decoder is unavailable or a bounded subprocess failed."""


def iq_to_cs8(samples: np.ndarray, sample_rate_hz: int) -> bytes:
    """Convert conventional complex IQ, normalized to converter full scale.

    20 MS/s passes through. 40 MS/s is antialias filtered and downsampled to
    20 MS/s. Samples are always independent of neighboring captures; missing
    samples are never bridged. Reject unsupported rates and nonfinite samples.
    """
    x = np.asarray(samples, dtype=np.complex64).reshape(-1)
    if not 128 <= len(x) <= 16380 or not np.isfinite(x).all():
        raise ValueError('IQ capture must contain 128..16380 finite samples')
    if sample_rate_hz == 40_000_000:
        x = np.convolve(x, _DOWNSAMPLE_FILTER, mode='same')[::2]
    elif sample_rate_hz != SAMPLE_RATE:
        raise ValueError('DSSS decoder accepts 20 or 40 MS/s IQ')
    raw = np.empty(2 * len(x), np.int8)
    raw[0::2] = np.clip(np.rint(x.real * 127.75), -128, 127).astype(np.int8)
    raw[1::2] = np.clip(np.rint(x.imag * 127.75), -128, 127).astype(np.int8)
    return raw.tobytes()


class DsssDecoder:
    """FCS-validated DSSS/CCK frames, isolated one input capture at a time.

    ``executable`` is the release build of crafter's radio_receive example.
    Configure explicitly or via ``WINJECT_CRAFTER_RADIO_RECEIVE``. Replayed
    inputs are independently bounded, and a timeout/error is surfaced as
    DsssUnavailable rather than silently reported as no frames.
    """
    def __init__(self, executable: str | os.PathLike | None = None, timeout: float = 2.0):
        binary = executable or os.environ.get('WINJECT_CRAFTER_RADIO_RECEIVE', '')
        self.executable = Path(binary).expanduser() if binary else None
        self.timeout = timeout

    @property
    def available(self) -> bool:
        return self.executable is not None and self.executable.is_file() and os.access(self.executable, os.X_OK)

    def decode(self, samples: np.ndarray, sample_rate_hz: int, frequency_hz: int = 2_412_000_000) -> list[dict]:
        """Return actual FCS-valid 802.11b MAC frames, never OFDM candidates.

        ``start_sample`` refers to source IQ rate and is an approximate decoder
        frame start. IQ around capture edges can be truncated; no frame is
        emitted without a complete valid FCS. An empty list means no frame was
        recovered and is not proof that the RF contained no 802.11 traffic.
        """
        if not self.available:
            raise DsssUnavailable('set WINJECT_CRAFTER_RADIO_RECEIVE to the built radio_receive executable')
        if not 1 <= frequency_hz <= 6_000_000_000:
            raise ValueError('frequency_hz out of range')
        cs8 = iq_to_cs8(samples, sample_rate_hz)
        # Unique private temp files prevent one client observing or replacing a
        # concurrent capture. The decoder is restarted to reset at every gap.
        with tempfile.NamedTemporaryFile(prefix='winject-dsss-', suffix='.cs8') as source:
            source.write(cs8)
            source.flush()
            try:
                result = subprocess.run(
                    [str(self.executable), '--replay', source.name, str(frequency_hz), '1', str(len(cs8)//2)],
                    stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=self.timeout, check=False,
                )
            except (OSError, subprocess.TimeoutExpired) as exc:
                raise DsssUnavailable(f'crafter decoder failed: {exc}') from exc
        if result.returncode:
            raise DsssUnavailable(f'crafter decoder exited {result.returncode}: {result.stderr[-256:].decode(errors="replace")}')
        decoded = []
        for line in result.stdout.splitlines():
            try:
                frame = json.loads(line)
            except (json.JSONDecodeError, UnicodeDecodeError) as exc:
                raise DsssUnavailable('crafter emitted invalid JSON') from exc
            if frame.get('kind') != 'frame' or frame.get('phy') not in ('dsss', 'cck'):
                continue
            if frame.get('fcs') != 'present_valid':
                continue
            try:
                mpdu_with_fcs = bytes.fromhex(frame['original_mac_hex'])
                metadata = parse_mpdu(mpdu_with_fcs)  # independently rechecks FCS
                rate = int(frame['rate_bps'])
                position = int(frame['position']['sample_index'])
                preamble = frame['preamble']
            except (KeyError, ValueError, TypeError) as exc:
                raise DsssUnavailable('crafter emitted invalid frame metadata') from exc
            if metadata is None:
                continue
            if rate not in (1_000_000, 2_000_000, 5_500_000, 11_000_000) or preamble not in ('short', 'long') or not 0 <= position < len(cs8)//2:
                raise DsssUnavailable('crafter emitted out-of-range frame metadata')
            decoded.append({**metadata,
                'detection': 'decoded_frame', 'status': 'decoded',
                'phy': frame['phy'], 'rate_bps': rate, 'preamble': preamble,
                'modulation': {1_000_000: 'DBPSK', 2_000_000: 'DQPSK', 5_500_000: 'CCK', 11_000_000: 'CCK'}[rate],
                'start_sample': position * (sample_rate_hz // SAMPLE_RATE),
                'evm_percent': None, 'constellation': [],
                'reason': '802.11b MPDU decoded by optional crafter receiver; FCS verified',
            })
        return decoded
