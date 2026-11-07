"""Detect 802.11 OFDM-like preambles in independent ESP32U IQ captures.

Detection uses repeated short/long training periods. It is deliberately a
candidate detector: neither frame validity, payload, modulation nor EVM follows
from periodicity alone. Captures have gaps and cannot be joined as a stream.
"""
from __future__ import annotations

import numpy as np

SUPPORTED_SAMPLE_RATE_HZ = 40_000_000

# Legacy 802.11 OFDM L-LTF BPSK subcarriers -26..26, including DC.
_LTF = np.array([1, 1, -1, -1, 1, 1, -1, 1, -1, 1, 1, 1, 1, 1, 1,
                 -1, -1, 1, 1, -1, 1, -1, 1, 1, 1, 1, 0, 1, -1, -1,
                 1, 1, -1, 1, -1, 1, -1, -1, -1, -1, -1, 1, 1, -1,
                 -1, 1, -1, 1, -1, 1, 1, 1, 1], dtype=np.float32)
_CARRIERS = np.r_[np.arange(-26, 0), np.arange(1, 27)]
_LTF_ACTIVE = _LTF[_LTF != 0]


def _training_metrics(x: np.ndarray, short_start: int, long_start: int,
                      long_score: float) -> dict:
    """Estimate L-LTF channel from first symbol, compare the independent second.

    Phase aligning the second symbol fits one degree of freedom, so this is an
    approximate L-LTF consistency/EVM estimate, not standards-compliant data
    EVM. A periodicity candidate is not itself proof of an 802.11 frame.
    """
    empty = {"training_evm_percent": None, "training_constellation": [],
             "channel_estimate": [], "channel_coherence": None,
             "carrier_offset_hz": None, "training_carriers": 0,
             "training_clipped_fraction": None,
             "training_warning": "L-LTF quality not sufficient for an estimate"}
    if long_score < .90 or long_start + 256 > len(x) or short_start + 288 > len(x):
        return empty
    s1 = x[short_start:short_start + 256]
    s2 = x[short_start + 32:short_start + 288]
    w = float(np.angle(np.vdot(s1, s2)) / 32.0)
    idx = np.arange(256)
    training = x[long_start:long_start + 256]
    clipped = float(np.mean((np.abs(training.real) >= .98) | (np.abs(training.imag) >= .98)))
    if clipped > .005:
        return {**empty, "training_clipped_fraction": round(clipped, 4),
                "training_warning": "L-LTF samples clipped; EVM estimate withheld"}
    corrected = training * np.exp(-1j * w * idx)
    y1 = np.fft.fft(corrected[:128])[_CARRIERS % 128]
    y2 = np.fft.fft(corrected[128:])[_CARRIERS % 128]
    phase = float(np.angle(np.vdot(y1, y2)))
    y2 *= np.exp(-1j * phase)
    h = y1 * _LTF_ACTIVE
    energy = np.abs(h) ** 2
    median = float(np.median(energy))
    if not np.isfinite(median) or median < 1e-10:
        return empty
    keep = energy > median * .04
    if np.count_nonzero(keep) < 40:
        return empty
    # Nearby subcarrier coherence helps reject arbitrary periodic waveforms;
    # skip the gap across DC. It is a heuristic, not a frame validation.
    left, right = np.r_[h[:25], h[26:51]], np.r_[h[1:26], h[27:52]]
    denom = float(np.sqrt(np.vdot(left, left).real * np.vdot(right, right).real))
    coherence = float(abs(np.vdot(left, right)) / max(denom, 1e-15))
    if not np.isfinite(coherence) or coherence < .70:
        return empty
    equalized = y2[keep] / h[keep]
    reference = _LTF_ACTIVE[keep]
    evm = float(100 * np.sqrt(np.mean(np.abs(equalized - reference) ** 2)))
    if not np.isfinite(evm) or evm > 100:
        return empty
    # Normalize channel magnitude for visualization; the ADC is not calibrated.
    normalized_h = h / np.sqrt(np.mean(np.abs(h) ** 2))
    return {
        "training_evm_percent": round(evm, 2),
        "training_constellation": [[round(float(z.real), 4), round(float(z.imag), 4)]
                                    for z in equalized],
        "channel_estimate": [[int(k), round(float(z.real), 4), round(float(z.imag), 4)]
                             for k, z in zip(_CARRIERS, normalized_h)],
        "channel_coherence": round(coherence, 4),
        "carrier_offset_hz": round(w * SUPPORTED_SAMPLE_RATE_HZ / (2 * np.pi), 1),
        "training_carriers": int(np.count_nonzero(keep)),
        "training_clipped_fraction": round(clipped, 4),
        "training_warning": "Approximate L-LTF-only EVM; candidate not decoded",
    }


def _moving_sum(values: np.ndarray, width: int) -> np.ndarray:
    dtype = np.complex128 if np.iscomplexobj(values) else np.float64
    cumsum = np.empty(len(values) + 1, dtype=dtype)
    cumsum[0] = 0
    np.cumsum(values, out=cumsum[1:])
    return cumsum[width:] - cumsum[:-width]


def _periodicity(samples: np.ndarray, lag: int, width: int) -> tuple[np.ndarray, np.ndarray]:
    a = samples[:-lag]
    b = samples[lag:]
    p = _moving_sum(a * b.conj(), width)
    ea = _moving_sum(np.abs(a) ** 2, width)
    eb = _moving_sum(np.abs(b) ** 2, width)
    return np.abs(p) ** 2 / (ea * eb + 1e-15), ea / width


def detect_frames(samples: np.ndarray, sample_rate_hz: int) -> list[dict]:
    """Return OFDM-like candidate preambles, with sample offsets in this burst.

    Only 40 MS/s is supported: 32- and 128-sample lags represent the 0.8 and
    3.2 microsecond repetitions in the 20 MHz legacy OFDM preamble. Thresholds
    are experimental, may return false positives and miss real frames, and must
    not be interpreted as frame decode or matched to ESP32 frame events.
    """
    if sample_rate_hz != SUPPORTED_SAMPLE_RATE_HZ:
        return []
    x = np.asarray(samples, dtype=np.complex64).reshape(-1)
    if len(x) < 1024 or not np.isfinite(x).all():
        return []
    short, power = _periodicity(x, 32, 128)
    long, _ = _periodicity(x, 128, 128)
    # Threshold corresponds to mean |IQ8|^2 > 16 before 10-bit normalization.
    short_ok = (short > 0.82) & (power > (16.0 / (127.75 ** 2)))
    edges = np.diff(np.concatenate(([False], short_ok, [False])).astype(np.int8))
    starts, ends = np.flatnonzero(edges == 1), np.flatnonzero(edges == -1)
    results = []
    last_start = -1024
    for begin, end in zip(starts, ends):
        if end - begin < 24:
            continue
        start = int(begin + np.argmax(short[begin:end]))
        lo, hi = start + 80, min(start + 600, len(long))
        if lo >= hi:
            continue
        long_start = int(lo + np.argmax(long[lo:hi]))
        long_score = float(long[long_start])
        if long_score < 0.82 or start - last_start < 256:
            continue
        training = _training_metrics(x, start, long_start, long_score)
        results.append({
            "start_sample": start,
            "long_start_sample": long_start,
            "detection": "ofdm_preamble_candidate",
            "short_score": round(float(short[start]), 4),
            "long_score": round(long_score, 4),
            "modulation": None,
            "mpdu_hex": None,
            "addr1": None,
            "addr2": None,
            "addr3": None,
            "evm_percent": None,
            "constellation": training["training_constellation"],
            **training,
            "status": "candidate",
            "reason": "OFDM-like preamble candidate; frame and symbols not decoded",
        })
        last_start = start
    return results
