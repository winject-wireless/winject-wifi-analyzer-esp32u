"""esp32-power-meter command line.

    python3 -m esp32_power_meter info
    python3 -m esp32_power_meter gain-scan --freq 2412
    python3 -m esp32_power_meter measure --freq 2412 --gain 20 --captures 50
    python3 -m esp32_power_meter calibrate --freq 2412 --gain 20 --known-dbm -30
    python3 -m esp32_power_meter spectrum --freq 2412 --gain 20   # debug, raw I/Q

Output is key=value lines so it can be grepped and diffed.
"""

from __future__ import annotations

import argparse
import sys
import time

import numpy as np

from .power import analyze, band_power_db, spectrum_db
from .protocol import EspSdr, SdrError


def _open(args) -> EspSdr:
    sdr = EspSdr(args.port, baud=args.baud)
    sdr.set_freq(args.freq)
    if args.gain is None:
        sdr.set_gain_hardware()
    else:
        sdr.set_gain_manual(args.gain)
    if args.bandwidth is not None:
        sdr.set_bandwidth(args.bandwidth)
    return sdr


def _collect(sdr: EspSdr, args, seconds: float):
    """Capture until `seconds` elapse; yield (capture, stats)."""
    t0 = time.monotonic()
    while time.monotonic() - t0 < seconds:
        cap = sdr.capture(args.samples, args.rate, args.bits)
        st = analyze(cap.iq, cap.full_scale, cap.rate_msps, threshold_db=args.threshold,
                     min_burst_us=args.min_burst_us)
        yield cap, st


def cmd_info(args) -> int:
    with EspSdr(args.port, baud=args.baud) as sdr:
        print(f"info={sdr.info()}")
        print(f"limits={sdr.limits()}")
        print(f"gain={sdr.gain()}")
    return 0


def _pwr_line(label: str, r: dict, offset) -> str:
    keys = ("bursts", "med", "p10", "p90", "min", "max", "noise", "clip_ppm", "len_us")
    line = label + " " + " ".join(f"{k}={r[k]:g}" for k in keys if k in r) + f" gain={r.get('gain')}"
    if offset is not None and r.get("bursts"):
        line += f" med_dbm={r['med'] + offset:.2f}"
    return line


def cmd_measure(args) -> int:
    """On-chip burst power (PWR); only the summary crosses the serial link."""
    with _open(args) as sdr:
        r = sdr.pwr(args.captures, args.rate, args.threshold, args.min_burst_us)
    print(f"freq_mhz={args.freq} rate_msps={args.rate} captures={args.captures}")
    if args.gain is None:
        print("warning=hardware AGC: readings are not comparable; use --gain")
    print(_pwr_line("result", r, args.offset_db))
    if r.get("clip_ppm", 0) > 100:
        print("warning=clipping: lower the gain index or add attenuation")
    return 0 if r.get("bursts") else 1


def cmd_gain_scan(args) -> int:
    """PWR at each gain index: find where readings move 1:1 and nothing clips."""
    with EspSdr(args.port, baud=args.baud) as sdr:
        lo, hi, step = sdr.limits()["gain"]
        sdr.set_freq(args.freq)
        if args.bandwidth is not None:
            sdr.set_bandwidth(args.bandwidth)
        for g in range(lo, hi + 1, max(1, step) * args.stride):
            sdr.set_gain_manual(g)
            r = sdr.pwr(args.captures, args.rate, args.threshold, args.min_burst_us)
            print(_pwr_line(f"gain={g}", r, None))
    return 0


def cmd_calibrate(args) -> int:
    """Offset for a known input: dBm = dBFS + offset at this gain and frequency."""
    with _open(args) as sdr:
        r = sdr.pwr(args.captures, args.rate, args.threshold, args.min_burst_us)
    if not r.get("bursts"):
        print("bursts=0")
        return 1
    offset = args.known_dbm - r["med"]
    print(_pwr_line("result", r, None))
    print(f"calibration freq_mhz={args.freq} gain={args.gain} known_dbm={args.known_dbm} "
          f"offset_db={offset:.2f}")
    return 0


def cmd_spectrum(args) -> int:
    """Average spectrum over captures that contain a burst, burst samples only."""
    specs = []
    with _open(args) as sdr:
        for cap, st in _collect(sdr, args, args.seconds):
            for b in st.bursts:
                seg = cap.iq[b.start: b.start + b.length]
                if len(seg) >= args.nfft:
                    specs.append(10 ** (spectrum_db(seg, cap.full_scale, args.nfft) / 10.0))
    if not specs:
        print("bursts=0")
        return 1
    spec = 10 * np.log10(np.mean(specs, axis=0))
    half = args.rate / 2
    total = band_power_db(spec, args.rate, -half, half)
    inband = band_power_db(spec, args.rate, -9.0, 9.0)
    dc = band_power_db(spec, args.rate, -0.2, 0.2)
    print(f"bursts={len(specs)} total_dbfs={total:.1f} inband_18mhz_dbfs={inband:.1f} "
          f"outband_db={total - inband:+.1f} dc_bin_dbfs={dc:.1f}")
    n = len(spec)
    for i in range(0, n, max(1, n // args.rows)):
        f = (i - n // 2) * args.rate / n
        print(f"bin_mhz={f:+.2f} dbfs={spec[i]:.1f}")
    return 0


def build_parser() -> argparse.ArgumentParser:
    ap = argparse.ArgumentParser(prog="esp32_power_meter")
    ap.add_argument("--port", default="/dev/ttyUSB0")
    ap.add_argument("--baud", type=int, default=2_000_000)
    sub = ap.add_subparsers(dest="cmd", required=True)

    def common(p):
        p.add_argument("--freq", type=int, default=2412, help="MHz")
        p.add_argument("--gain", type=int, default=None, help="manual gain index (default: hardware AGC)")
        p.add_argument("--bandwidth", type=int, default=None, help="MHz, 0 = widest")
        p.add_argument("--rate", type=float, default=80.0, choices=(80.0, 40.0, 16.0), help="MS/s (ESP32: 80, 40, 16)")
        p.add_argument("--bits", type=int, choices=(16, 20), default=20)
        p.add_argument("--samples", type=int, default=16380)
        p.add_argument("--seconds", type=float, default=5.0)
        p.add_argument("--threshold", type=int, default=10, help="burst threshold above noise, dB")
        p.add_argument("--min-burst-us", type=int, default=20)

    def onchip(p):
        p.add_argument("--captures", type=int, default=20, help="snapshots per PWR (1-500)")

    sub.add_parser("info").set_defaults(func=cmd_info)
    p = sub.add_parser("measure"); common(p); onchip(p)
    p.add_argument("--offset-db", type=float, default=None, help="calibration: dBm = dBFS + offset")
    p.set_defaults(func=cmd_measure)
    p = sub.add_parser("gain-scan"); common(p); onchip(p)
    p.add_argument("--stride", type=int, default=1)
    p.set_defaults(func=cmd_gain_scan, captures=5)
    p = sub.add_parser("calibrate", help="needs --gain: an AGC reading has no fixed offset"); common(p); onchip(p)
    p.add_argument("--known-dbm", type=float, required=True, help="true input power at the SDR port")
    p.set_defaults(func=cmd_calibrate)
    p = sub.add_parser("spectrum", help="debug: raw captures to the host"); common(p)
    p.add_argument("--nfft", type=int, default=256)
    p.add_argument("--rows", type=int, default=32)
    p.set_defaults(func=cmd_spectrum)
    return ap


def main(argv=None) -> int:
    ap = build_parser()
    args = ap.parse_args(argv)
    if args.cmd == "calibrate" and args.gain is None:
        ap.error("calibrate needs --gain (manual gain index)")
    try:
        return args.func(args)
    except SdrError as e:
        print(f"error={e}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
