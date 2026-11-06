import struct
import unittest
import zlib

import numpy as np

from winject_wifi_analyzer import dsp
from winject_wifi_analyzer.protocol import (FLAG_LAST_FRAGMENT, HEADER, IQHeader,
                                            ProtocolError, StreamAssembler)


def frame(capture_id, samples, bits, rate, offset, payload, flags=0, freq_khz=2437000,
          step=0, steps=1, job=1):
    crc = zlib.crc32(payload) & 0xFFFFFFFF
    return HEADER.pack(b"WJIQ", 2, flags, bits, rate, capture_id, samples, offset,
                       len(payload), HEADER.size, crc, freq_khz, 1234, 40, 0, job,
                       step, steps) + payload


class ProtocolTest(unittest.TestCase):
    def test_header_is_44_bytes(self):
        self.assertEqual(HEADER.size, 44)

    def test_reassembles_out_of_order_and_duplicate(self):
        payload = bytes(range(16))
        asm = StreamAssembler()
        self.assertIsNone(asm.add(frame(7, 8, 8, 6, 8, payload[8:], FLAG_LAST_FRAGMENT)))
        self.assertIsNone(asm.add(frame(7, 8, 8, 6, 8, payload[8:], FLAG_LAST_FRAGMENT)))
        cap = asm.add(frame(7, 8, 8, 6, 0, payload[:8]))
        self.assertEqual(cap.payload, payload)
        self.assertEqual(cap.frequency_hz, 2_437_000_000)
        self.assertEqual(cap.sample_rate_hz, 16_000_000)

    def test_rejects_bad_crc(self):
        d = bytearray(frame(3, 4, 8, 0, 0, bytes(range(8)), FLAG_LAST_FRAGMENT))
        d[-1] ^= 0xFF
        with self.assertRaisesRegex(ProtocolError, "CRC"):
            IQHeader.decode(bytes(d))
        asm = StreamAssembler()
        self.assertIsNone(asm.add(bytes(d)))
        self.assertEqual(asm.errors, 1)

    def test_rejects_truncated_datagram(self):
        with self.assertRaisesRegex(ProtocolError, "truncated"):
            IQHeader.decode(b"WJIQ")

    def test_counts_lost_captures(self):
        asm = StreamAssembler(window=2)
        for cid in range(1, 5):  # first fragment only: never completes
            asm.add(frame(cid, 8, 8, 6, 0, bytes(8)))
        self.assertEqual(asm.lost, 2)

    def test_iq8_samples_conventional_sign(self):
        # I = 64 (x4 = 256), raw Q = 32 (x4 = 128) -> Q conventional = -128.
        payload = struct.pack("bb", 64, 32) * 4
        cap = StreamAssembler().add(frame(1, 4, 8, 6, 0, payload, FLAG_LAST_FRAGMENT))
        x = cap.samples()
        np.testing.assert_allclose(x.real, 256 / 511, rtol=1e-6)
        np.testing.assert_allclose(x.imag, -128 / 511, rtol=1e-6)

    def test_iq10_unpack(self):
        # Two samples: (I=-1, Qraw=5) and (I=100, Qraw=-200), CAP20 packing.
        def w(i, q):
            return (i & 0x3FF) | ((q & 0x3FF) << 10)
        a, c = w(-1, 5), w(100, -200)
        v = a | (c << 20)
        payload = v.to_bytes(5, "little")
        cap = StreamAssembler().add(frame(2, 2, 10, 1, 0, payload, FLAG_LAST_FRAGMENT))
        x = cap.samples() * 511
        np.testing.assert_allclose(x, [-1 - 5j, 100 + 200j], atol=1e-3)


class DspTest(unittest.TestCase):
    def test_full_scale_tone_is_0_dbfs(self):
        fs, n = 16e6, 4096
        t = np.arange(n) / fs
        x = np.exp(2j * np.pi * 1e6 * t).astype(np.complex64)
        st = dsp.power_stats(x, fs)
        self.assertAlmostEqual(st.mean_dbfs, 0.0, places=3)
        self.assertAlmostEqual(st.tone_offset_hz, 1e6, delta=fs / 1024)
        self.assertAlmostEqual(st.tone_dbfs, 0.0, delta=0.2)

    def test_sweep_tile_keeps_central_step(self):
        fs = 16e6
        x = (np.random.default_rng(1).standard_normal(1024) * 0.01).astype(np.complex64)
        tile = dsp.sweep_tile([x, x], fs, 2.4e9, 10e6, 512, False)
        span = tile["bin_hz"] * len(tile["bins"])
        self.assertAlmostEqual(span, 10e6, delta=2 * fs / 512)
        self.assertLess(tile["start_hz"], 2.4e9)


if __name__ == "__main__":
    unittest.main()
