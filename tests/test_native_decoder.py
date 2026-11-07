import unittest
import numpy as np
from winject_wifi_analyzer.native_decoder import viterbi, _python_viterbi, BACKEND

class NativeViterbiTests(unittest.TestCase):
    def test_descrambler_matches_reference_for_all_seeds(self):
        import winject_wifi_analyzer.legacy_ofdm as phy
        if phy._native_descramble is None:
            self.skipTest('C++ extension not built')
        rng=np.random.default_rng(9)
        for seed in range(1,128):
            bits=rng.integers(0,2,200,dtype=np.uint8); state=seed
            for i in range(16):
                feedback=((state>>6)^(state>>3))&1
                bits[i]=feedback; state=((state<<1)|feedback)&127
            native=phy._descramble(bits); saved=phy._native_descramble
            phy._native_descramble=None
            try: reference=phy._descramble(bits)
            finally: phy._native_descramble=saved
            np.testing.assert_array_equal(native,reference)
    def test_matches_reference_with_punctures(self):
        rng=np.random.default_rng(12345)
        for size in (0,2,48,127,1000):
            for final in (False,True):
                coded=rng.integers(0,3,size*2,dtype=np.uint8)
                np.testing.assert_array_equal(viterbi(coded,final),_python_viterbi(coded,final))
    def test_rejects_invalid_input(self):
        with self.assertRaises(ValueError):viterbi(np.array([1],dtype=np.uint8),False)
        with self.assertRaises(ValueError):viterbi(np.array([0,3],dtype=np.uint8),False)
    def test_compiled_backend_is_installed_for_live_run(self):
        # A developer without native build can still run the reference tests.
        self.assertIn(BACKEND,('native-cpp','python'))
