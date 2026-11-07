import unittest

import numpy as np

from winject_wifi_analyzer.frame_detector import (
    _CARRIERS, _LTF_ACTIVE, _training_metrics, detect_frames,
)


class FrameDetectorTests(unittest.TestCase):
    def setUp(self):
        rng = np.random.default_rng(35)
        self.short = (rng.standard_normal(32) + 1j * rng.standard_normal(32)).astype(np.complex64)
        freq = np.zeros(128, np.complex64)
        freq[_CARRIERS % 128] = _LTF_ACTIVE
        self.ltf = np.fft.ifft(freq).astype(np.complex64) * 3
        self.signal = np.r_[np.tile(self.short, 10), np.zeros(64, np.complex64), self.ltf, self.ltf, np.zeros(500, np.complex64)]

    def test_first_ltf_channel_second_ltf_evm(self):
        good = _training_metrics(self.signal, 0, 384, 1)
        self.assertIsNotNone(good['training_evm_percent'])
        self.assertLess(good['training_evm_percent'], 0.1)
        rng = np.random.default_rng(123)
        corrupted = self.signal.copy()
        noise = (rng.standard_normal(128) + 1j*rng.standard_normal(128)) * 0.03
        corrupted[512:640] += noise
        bad = _training_metrics(corrupted, 0, 384, 1)
        self.assertIsNotNone(bad['training_evm_percent'])
        self.assertGreater(bad['training_evm_percent'], good['training_evm_percent'] + 1)
        self.assertEqual(len(good['channel_estimate']), 52)

    def test_withholds_training_evm_for_clipped_ltf(self):
        clipped = self.signal.copy()
        clipped[384:390] = 1 + 1j
        result = _training_metrics(clipped, 0, 384, 1)
        self.assertIsNone(result['training_evm_percent'])
        self.assertIn('clipped', result['training_warning'])

    def test_refuses_invalid_and_unsupported_rates(self):
        self.assertEqual(detect_frames(self.signal, 16_000_000), [])
        broken = self.signal.copy()
        broken[100] = np.nan
        self.assertEqual(detect_frames(broken, 40_000_000), [])
        self.assertIsNone(_training_metrics(self.signal, 0, 384, .5)['training_evm_percent'])

    def test_noise_does_not_yield_candidates(self):
        rng = np.random.default_rng(7)
        for _ in range(10):
            noise = (rng.normal(0, .2, 16380) + 1j*rng.normal(0, .2, 16380)).astype('complex64')
            self.assertEqual(detect_frames(noise, 40_000_000), [])


if __name__ == '__main__':
    unittest.main()
