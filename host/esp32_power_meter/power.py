"""Burst power analysis of fixed-gain I/Q captures.

Power is in dBFS: 0 dBFS is a sample whose magnitude equals the converter's
full scale. With the receiver gain fixed, dBFS differences are input power
differences; an offset (dBm = dBFS + offset) comes from calibration.
"""

from __future__ import annotations

from dataclasses import dataclass, field

import numpy as np


def to_db(p: np.ndarray | float) -> np.ndarray | float:
    return 10.0 * np.log10(np.maximum(p, 1e-20))


@dataclass
class Burst:
    start: int          # sample index
    length: int         # samples
    power_dbfs: float   # mean power over the trimmed burst
    peak_dbfs: float    # highest single-sample power
    clip_frac: float    # share of samples at the converter rail


@dataclass
class CaptureStats:
    noise_dbfs: float
    bursts: list[Burst] = field(default_factory=list)
    clip_frac: float = 0.0


def moving_average(x: np.ndarray, n: int) -> np.ndarray:
    if n <= 1:
        return x
    k = np.ones(n, dtype=np.float64) / n
    return np.convolve(x, k, mode="same")


def analyze(iq: np.ndarray, full_scale: float, rate_msps: float,
            threshold_db: float = 10.0, min_burst_us: float = 20.0,
            smooth_us: float = 1.0, trim: float = 0.1,
            rail_frac: float = 0.98) -> CaptureStats:
    """Find bursts that rise `threshold_db` above the noise floor.

    The noise floor is the 20th percentile of the smoothed power, so it holds
    while bursts occupy most of the capture. Burst power is the mean of the raw
    sample power over the burst with `trim` of its length cut from each end
    (ramp-up and ramp-down).
    """
    p = (np.abs(iq.astype(np.complex128)) / full_scale) ** 2
    rail = np.maximum(np.abs(iq.real), np.abs(iq.imag)) >= rail_frac * full_scale
    smooth = moving_average(p, max(1, int(round(smooth_us * rate_msps))))
    noise = float(np.percentile(smooth, 20))
    stats = CaptureStats(noise_dbfs=float(to_db(noise)), clip_frac=float(rail.mean()))

    above = smooth > noise * 10 ** (threshold_db / 10.0)
    if not above.any():
        return stats
    edges = np.diff(above.astype(np.int8), prepend=0, append=0)
    starts = np.flatnonzero(edges == 1)
    ends = np.flatnonzero(edges == -1)
    min_len = int(min_burst_us * rate_msps)
    for s, e in zip(starts, ends):
        n = e - s
        if n < min_len:
            continue
        cut = int(n * trim)
        seg = slice(s + cut, e - cut)
        stats.bursts.append(Burst(
            start=int(s), length=int(n),
            power_dbfs=float(to_db(p[seg].mean())),
            peak_dbfs=float(to_db(p[s:e].max())),
            clip_frac=float(rail[s:e].mean()),
        ))
    return stats


def spectrum_db(iq: np.ndarray, full_scale: float, nfft: int = 256) -> np.ndarray:
    """Welch-averaged power spectrum in dBFS per bin, DC in the middle."""
    x = iq.astype(np.complex128) / full_scale
    segs = len(x) // nfft
    if segs == 0:
        raise ValueError("capture shorter than one FFT")
    w = np.hanning(nfft)
    frames = x[: segs * nfft].reshape(segs, nfft) * w
    # Scaled so the bins sum to the mean sample power (Parseval).
    psd = (np.abs(np.fft.fft(frames, axis=1)) ** 2).mean(axis=0) / (nfft * (w ** 2).sum())
    return to_db(np.fft.fftshift(psd))


def band_power_db(spec_db: np.ndarray, rate_msps: float, lo_mhz: float, hi_mhz: float) -> float:
    """Total power in [lo, hi) MHz offset from the tuned centre, in dBFS."""
    n = len(spec_db)
    f = (np.arange(n) - n // 2) * rate_msps / n
    sel = (f >= lo_mhz) & (f < hi_mhz)
    return float(to_db((10 ** (spec_db[sel] / 10.0)).sum()))
