import os
import sys
import contextlib
import io
import unittest

import numpy as np

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "host"))

from esp32_power_meter.power import analyze, band_power_db, spectrum_db  # noqa: E402
from esp32_power_meter.protocol import (decode_cap16, decode_cap20, parse_pwr, payload_len,  # noqa: E402
                                        rate_code)
from esp32_power_meter import cli  # noqa: E402


def pack_cap20(i_vals, negq_vals):
    """Reference packer: I in bits 0-9, -Q in bits 10-19, LSB first, no padding."""
    acc, nbits, out = 0, 0, bytearray()
    for i, q in zip(i_vals, negq_vals):
        word = (i & 0x3FF) | ((q & 0x3FF) << 10)
        acc |= word << nbits
        nbits += 20
        while nbits >= 8:
            out.append(acc & 0xFF)
            acc >>= 8
            nbits -= 8
    if nbits:
        out.append(acc & 0xFF)
    return bytes(out)


class ProtocolTest(unittest.TestCase):
    def test_payload_len(self):
        self.assertEqual(payload_len(16380, 20), 40950)
        self.assertEqual(payload_len(3, 20), 8)      # 60 bits -> 8 bytes
        self.assertEqual(payload_len(16380, 16), 32760)

    def test_cap20_roundtrip_with_q_negation(self):
        i = [0, 1, -1, 511, -512, 100, -37]
        q = [5, -5, 0, -511, 511, -100, 37]          # standard Q (-Q must fit in 10 bits)
        data = pack_cap20(i, [-v for v in q])         # firmware sends -Q
        iq = decode_cap20(data, len(i))
        np.testing.assert_array_equal(iq.real, i)
        np.testing.assert_array_equal(iq.imag, q)

    def test_cap16(self):
        raw = np.array([10, -20, -128, 127], dtype=np.int8).tobytes()
        iq = decode_cap16(raw, 2)
        np.testing.assert_array_equal(iq.real, [10, -128])
        np.testing.assert_array_equal(iq.imag, [20, -127])

    def test_parse_pwr(self):
        r = parse_pwr("PWR caps=10 bursts=10 med=-20.00 p10=-20.06 p90=-19.99 min=-20.10 "
                      "max=-19.96 peak=-8.25 noise=-49.63 clip_ppm=0 len_us=121.96 gain=MANUAL_40")
        self.assertEqual(r["bursts"], 10)
        self.assertAlmostEqual(r["med"], -20.0)
        self.assertEqual(r["gain"], "MANUAL_40")
        c = parse_pwr("PWRC caps=20 med=-22.10 p10=-22.15 p90=-22.05 clip_ppm=0 gain=MANUAL_18", "PWRC")
        self.assertAlmostEqual(c["med"], -22.1)
        with self.assertRaises(Exception):
            parse_pwr("PWRC caps=1 med=-1", "PWR")
        empty = parse_pwr("PWR caps=3 bursts=0 med=nan p10=nan noise=-50.13 gain=HARDWARE")
        self.assertNotEqual(empty["med"], empty["med"])  # NaN

    def test_cli_parses(self):
        ap = cli.build_parser()
        a = ap.parse_args(["measure", "--gain", "20", "--captures", "5"])
        self.assertEqual((a.func, a.gain, a.captures, a.rate), (cli.cmd_measure, 20, 5, 80.0))
        a = ap.parse_args(["gain-scan", "--stride", "4"])
        self.assertEqual((a.func, a.captures, a.stride), (cli.cmd_gain_scan, 5, 4))
        a = ap.parse_args(["cw", "--freq", "2447", "--gain", "18", "--known-dbm", "-40"])
        self.assertEqual((a.func, a.freq, a.known_dbm), (cli.cmd_cw, 2447, -40.0))
        a = ap.parse_args(["calibrate", "--gain", "20", "--known-dbm", "-30"])
        self.assertEqual((a.func, a.known_dbm), (cli.cmd_calibrate, -30.0))
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            ap.parse_args(["measure", "--rate", "20"])   # not an ESP32 rate
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            cli.main(["calibrate", "--known-dbm", "-30"])  # no --gain

    def test_rate_code(self):
        self.assertEqual(rate_code(20), 2)
        self.assertEqual(rate_code(80), 0)
        with self.assertRaises(ValueError):
            rate_code(25)


class PowerTest(unittest.TestCase):
    rate = 20.0
    fs = 511.0

    def signal(self, burst_dbfs, noise_dbfs=-50.0, n=16380, start=4000, length=6000, seed=1):
        rng = np.random.default_rng(seed)
        def cn(power, size):
            return np.sqrt(power / 2) * (rng.standard_normal(size) + 1j * rng.standard_normal(size))
        x = cn(10 ** (noise_dbfs / 10), n)
        x[start:start + length] += cn(10 ** (burst_dbfs / 10), length)
        return (x * self.fs).astype(np.complex64)

    def test_burst_power_and_noise(self):
        st = analyze(self.signal(-20.0), self.fs, self.rate)
        self.assertEqual(len(st.bursts), 1)
        b = st.bursts[0]
        self.assertAlmostEqual(b.power_dbfs, -20.0, delta=0.3)
        self.assertAlmostEqual(st.noise_dbfs, -50.0, delta=1.5)
        self.assertLess(abs(b.start - 4000), 40)
        self.assertLess(abs(b.length - 6000), 80)
        self.assertEqual(st.clip_frac, 0.0)

    def test_linearity(self):
        """A 6 dB step in input must read as 6 dB."""
        a = analyze(self.signal(-30.0), self.fs, self.rate).bursts[0].power_dbfs
        b = analyze(self.signal(-24.0), self.fs, self.rate).bursts[0].power_dbfs
        self.assertAlmostEqual(b - a, 6.0, delta=0.3)

    def test_no_burst(self):
        st = analyze(self.signal(-100.0), self.fs, self.rate)
        self.assertEqual(st.bursts, [])

    def test_clipping_detected(self):
        x = self.signal(-20.0)
        x[5000:5100] = self.fs + 0j
        st = analyze(x, self.fs, self.rate)
        self.assertGreater(st.bursts[0].clip_frac, 0.0)

    def test_spectrum_tone(self):
        n = 8192
        t = np.arange(n)
        x = (0.1 * self.fs * np.exp(2j * np.pi * 5.0 / self.rate * t)).astype(np.complex64)
        spec = spectrum_db(x, self.fs, 256)
        peak = (np.argmax(spec) - 128) * self.rate / 256
        self.assertAlmostEqual(peak, 5.0, delta=0.1)
        self.assertAlmostEqual(band_power_db(spec, self.rate, 4.0, 6.0), -20.0, delta=0.5)


if __name__ == "__main__":
    unittest.main()
