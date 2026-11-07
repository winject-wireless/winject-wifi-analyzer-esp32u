"""DSSS adapter contract plus optional independent MIT libcrafter vectors.

For integration vectors set WINJECT_CRAFTER_RADIO_RECEIVE to the built
executable and WINJECT_CRAFTER_FIXTURES to crafter/tests/fixtures/iq in the
pinned upstream checkout. No fixture or executable is required for unit tests.
"""
import os
from pathlib import Path
import unittest

import numpy as np

from winject_wifi_analyzer.dsss import DsssDecoder, DsssUnavailable, iq_to_cs8


class DsssAdapterTests(unittest.TestCase):
    def test_unavailable_is_distinct_from_no_frame(self):
        d = DsssDecoder('/this/path/does/not/exist')
        self.assertFalse(d.available)
        with self.assertRaises(DsssUnavailable):
            d.decode(np.ones(1000, np.complex64), 40_000_000)

    def test_reject_unsupported_and_invalid_iq(self):
        with self.assertRaises(ValueError):
            iq_to_cs8(np.ones(1000, np.complex64), 16_000_000)
        x = np.ones(1000, np.complex64)
        x[20] = np.nan
        with self.assertRaises(ValueError):
            iq_to_cs8(x, 40_000_000)

    def test_resampling_length_and_no_wraparound(self):
        x = np.full(1000, 1.2 + 1.2j, np.complex64)
        y = np.frombuffer(iq_to_cs8(x, 40_000_000), np.int8).reshape(-1, 2)
        self.assertEqual(y.shape, (500, 2))
        self.assertTrue((y[100:-100] == 127).all())


@unittest.skipUnless(os.getenv('WINJECT_CRAFTER_RADIO_RECEIVE') and os.getenv('WINJECT_CRAFTER_FIXTURES'),
                     'requires explicitly installed pinned libcrafter test vectors and binary')
class DsssIndependentVectors(unittest.TestCase):
    def setUp(self):
        self.decoder = DsssDecoder()
        self.root = Path(os.environ['WINJECT_CRAFTER_FIXTURES'])

    def load(self, name):
        iq = np.fromfile(self.root / (name + '.cs8'), np.int8).reshape(-1, 2).astype('float32')
        return (iq[:, 0] + 1j*iq[:, 1]) / 127.75

    def test_rate_matrix_and_bad_fcs(self):
        cases = [('dsss-10-long-clean-48', 1_000_000),
                 ('dsss-20-long-clean-48', 2_000_000),
                 ('dsss-55-long-clean-48', 5_500_000),
                 ('dsss-110-short-clean-48', 11_000_000)]
        for name, rate in cases:
            with self.subTest(name=name):
                frames = self.decoder.decode(self.load(name), 20_000_000)
                self.assertEqual(len(frames), 1)
                self.assertEqual(frames[0]['rate_bps'], rate)
                self.assertTrue(frames[0]['fcs_ok'])
        self.assertEqual(self.decoder.decode(self.load('dsss-10-long-bad_fcs-48'), 20_000_000), [])

    def test_real_device_40_msps_resampling_contract(self):
        try:
            from scipy.signal import resample_poly
        except ImportError:
            self.skipTest('test-only dependency scipy is unavailable')
        # 48-byte 2 Mbps long preamble frame is ~388 us, which fits in one
        # 16,380-sample burst; the 1 Mbps counterpart exceeds the burst.
        for name, rate in [('dsss-20-long-clean-48', 2_000_000),
                           ('dsss-55-long-clean-48', 5_500_000),
                           ('dsss-110-short-clean-48', 11_000_000)]:
            with self.subTest(name=name):
                samples = resample_poly(self.load(name), 2, 1).astype('complex64')
                self.assertLessEqual(len(samples), 16380)
                frames = self.decoder.decode(samples, 40_000_000)
                self.assertEqual(len(frames), 1)
                self.assertTrue(frames[0]['fcs_ok'])
                self.assertEqual(frames[0]['rate_bps'], rate)
        signal = resample_poly(self.load('dsss-10-long-clean-48'), 2, 1).astype('complex64')
        self.assertGreater(len(signal), 16380)
        self.assertEqual(self.decoder.decode(signal[:16380], 40_000_000), [])


if __name__ == '__main__':
    unittest.main()
