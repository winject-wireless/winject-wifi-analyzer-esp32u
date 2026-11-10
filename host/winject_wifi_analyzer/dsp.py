"""Host-side signal processing for ESP32U IQ captures.

Samples are complex64 with |x| = 1 at 0 dBFS (protocol.Capture.samples()).
"""
from __future__ import annotations

from dataclasses import dataclass

import numpy as np

CLIP = 501.0 / 511.0  # |I| or |Q| at the converter rail (98 %)


def db(p: np.ndarray | float) -> np.ndarray | float:
    return 10.0 * np.log10(np.maximum(p, 1e-12))


@dataclass(frozen=True)
class PowerStats:
    mean_dbfs: float
    peak_dbfs: float
    dc_dbfs: float
    clipped: float
    tone_offset_hz: float
    tone_dbfs: float


def power_stats(x: np.ndarray, fs: float, nfft: int = 1024) -> PowerStats:
    p = (x.real * x.real + x.imag * x.imag)
    dc = x.mean()
    clipped = float(np.mean((np.abs(x.real) >= CLIP) | (np.abs(x.imag) >= CLIP)))
    tone_hz, tone_db = 0.0, -120.0
    if len(x) >= nfft:
        freqs, spec = psd(x[:nfft], fs, nfft)
        mid = nfft // 2
        spec = spec.copy()
        spec[mid - 1:mid + 2] = -200.0  # DC residue
        k = int(np.argmax(spec))
        tone_hz, tone_db = float(freqs[k]), float(spec[k])
    return PowerStats(float(db(p.mean())), float(db(p.max())), float(db(abs(dc) ** 2)),
                      clipped, tone_hz, tone_db)


_windows: dict[int, tuple[np.ndarray, float]] = {}


def _hann(n: int) -> tuple[np.ndarray, float]:
    w = _windows.get(n)
    if w is None:
        win = np.hanning(n).astype(np.float32)
        w = _windows[n] = (win, float(win.sum()))
    return w


def psd(x: np.ndarray, fs: float, nfft: int = 1024, peak: bool = False
        ) -> tuple[np.ndarray, np.ndarray]:
    """Welch spectrum in dBFS per bin (full-scale tone = 0 dBFS), DC removed,
    centered (fftshift). Segments of nfft with 50 % overlap; mean or max."""
    win, wsum = _hann(nfft)
    x = x - x.mean()
    if len(x) < nfft:
        x = np.concatenate([x, np.zeros(nfft - len(x), np.complex64)])
    hop = nfft // 2
    starts = range(0, len(x) - nfft + 1, hop)
    segs = np.stack([x[s:s + nfft] for s in starts]) * win
    spec = np.abs(np.fft.fft(segs, axis=1)) ** 2 / (wsum * wsum)
    p = spec.max(axis=0) if peak else spec.mean(axis=0)
    p = np.fft.fftshift(p)
    freqs = (np.arange(nfft) - nfft // 2) * (fs / nfft)
    return freqs, db(p).astype(np.float32)


def envelope(x: np.ndarray, buckets: int = 1024) -> np.ndarray:
    """Max |x| per bucket in dBFS: shows bursts/frames across the capture."""
    mag = np.abs(x)
    n = (len(mag) // buckets) * buckets
    if n == 0:
        return db(mag ** 2).astype(np.float32)
    return db(mag[:n].reshape(buckets, -1).max(axis=1) ** 2).astype(np.float32)


def decimate_points(x: np.ndarray, count: int) -> np.ndarray:
    """A contiguous run of samples from the middle (keeps waveforms readable)."""
    if len(x) <= count:
        return x
    start = (len(x) - count) // 2
    return x[start:start + count]


def merge_bins(freqs: np.ndarray, spec_db: np.ndarray, max_bins: int, peak: bool
               ) -> tuple[np.ndarray, np.ndarray]:
    if len(spec_db) <= max_bins:
        return freqs, spec_db
    g = int(np.ceil(len(spec_db) / max_bins))
    n = (len(spec_db) // g) * g
    lin = 10 ** (spec_db[:n].reshape(-1, g) / 10)
    merged = lin.max(axis=1) if peak else lin.mean(axis=1)
    return freqs[:n].reshape(-1, g).mean(axis=1), db(merged).astype(np.float32)


def sweep_tile(captures: list[np.ndarray], fs: float, center_hz: float, step_hz: float,
               nfft: int, peak: bool, max_bins: int = 256, *, crop_offset_hz: float = 0) -> dict:
    """Select one sweep tile from the averaged PSD around the crop offset."""
    acc = None
    for x in captures:
        _, s = psd(x, fs, nfft, peak)
        lin = 10 ** (s / 10)
        acc = lin if acc is None else (np.maximum(acc, lin) if peak else acc + lin)
    lin = acc if peak else acc / len(captures)
    freqs = (np.arange(nfft) - nfft // 2) * (fs / nfft)
    keep = np.abs(freqs - crop_offset_hz) <= step_hz / 2
    f, s = merge_bins(freqs[keep] + center_hz, db(lin[keep]).astype(np.float32),
                      max_bins, peak)
    return {
        "frequency_hz": center_hz,
        "start_hz": float(f[0]),
        "bin_hz": float(f[1] - f[0]) if len(f) > 1 else step_hz,
        "bins": [round(float(v), 1) for v in s],
        "power_dbfs": round(float(db(lin[keep].sum())), 1),
        "peak_dbfs": round(float(s.max()), 1),
    }
