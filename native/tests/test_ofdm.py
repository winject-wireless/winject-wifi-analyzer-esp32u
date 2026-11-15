"""Compile the standalone C++ decoder and validate known IQ captures.

Run from repository root: PYTHONPATH=host:tests python3 native/tests/test_ofdm.py
Requires numpy and scipy for independent fixture conversion; receiver itself has
no Python dependency. The fixture provenance is in tests/fixtures/openofdm.
"""
import pathlib
import subprocess
import tempfile
import unittest
from unittest.mock import patch
import numpy as np
from scipy.signal import resample_poly
from test_legacy_ofdm import make_frame

ROOT = pathlib.Path(__file__).resolve().parents[2]

class NativeOfdmReference(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.exe = pathlib.Path(cls.temp.name) / 'ofdm-test'
        cls.scalar = pathlib.Path(cls.temp.name) / 'ofdm-test-scalar'
        for exe, flags in ((cls.exe, []), (cls.scalar, ['-U__ARM_NEON'])):
            subprocess.run(['c++', '-O3', '-std=c++17', *flags, '-I', str(ROOT/'native'),
                            str(ROOT/'native/tests/ofdm_reference_test.cpp'),
                            str(ROOT/'native/ofdm.cpp'), '-o', str(exe)], check=True)

    @classmethod
    def tearDownClass(cls): cls.temp.cleanup()

    def decode(self, samples, short=360, long=584):
        path = pathlib.Path(self.temp.name) / 'samples.cf32'
        np.asarray(samples, np.complex64).tofile(path)
        outputs=[]
        for exe in (self.exe,self.scalar):
            r=subprocess.run([exe,path,str(short),str(long)],capture_output=True,text=True,check=True)
            outputs.append([int(line.split()[0]) for line in r.stdout.splitlines()])
        self.assertEqual(outputs[0],outputs[1], 'SIMD and scalar decoded different rates')
        return outputs[0]

    def test_pre_pilot_constellation_preserves_payload_phase(self):
        import test_legacy_ofdm as reference
        mpdu=bytes.fromhex('08000000 ffffffffffff 102030405060 aabbccddeeff 3012')+b'phase test'
        original=reference.symbol
        calls=0
        def rotated(*args, **kwargs):
            nonlocal calls
            calls += 1
            out=original(*args, **kwargs)
            return out if calls == 1 else out * np.exp(0.4j)
        with patch('test_legacy_ofdm.symbol', side_effect=rotated):
            samples,short,long=make_frame(mpdu,rate=24)
        path=pathlib.Path(self.temp.name)/'phase.cf32'
        np.asarray(samples,np.complex64).tofile(path)
        for exe in (self.exe,self.scalar):
            result=subprocess.run([exe,path,str(short),str(long)],capture_output=True,text=True,check=True)
            lines=[line for line in result.stderr.splitlines() if line.startswith('iq ')]
            self.assertTrue(lines)
            _,ri,rq,ci,cq,training=lines[0].split()
            phase=np.angle(complex(float(ri),float(rq))/complex(float(ci),float(cq)))
            self.assertAlmostEqual(phase,0.4,delta=0.03)
            self.assertEqual(int(training),104)

    def test_independent_openofdm_all_rates(self):
        root=ROOT/'tests/fixtures/openofdm'
        for rate in (6,9,12,18,24,36,48,54):
            with self.subTest(rate=rate):
                if rate in (6,54):
                    raw=np.loadtxt(root/f'ag_{rate}M_len14_pre100_post200_openwifi.txt')
                else:
                    path=next(root.glob(f'dot11a_{rate}mbps_*.dat'))
                    raw=np.fromfile(path,dtype='<i2').astype(np.float32).reshape(-1,2)
                samples=resample_poly(raw[:,0]+1j*raw[:,1],2,1).astype(np.complex64)/32768
                self.assertIn(rate,self.decode(samples))

    def test_generated_all_rates_bad_fcs_and_truncation(self):
        mpdu=bytes.fromhex('08000000 ffffffffffff 102030405060 aabbccddeeff 3012')+b'legacy OFDM'
        for rate in (6,9,12,18,24,36,48,54):
            with self.subTest(rate=rate):
                samples,short,long=make_frame(mpdu,rate=rate)
                self.assertIn(rate,self.decode(samples,short,long))
                self.assertNotIn(rate,self.decode(samples[:-300],short,long))
                bad,short,long=make_frame(mpdu,rate=rate,corrupt_fcs=True)
                self.assertNotIn(rate,self.decode(bad,short,long))

    def test_long_frames_use_the_whole_pilot_polarity_sequence(self):
        # Pilot polarity entries past index 68 were wrong, so every frame longer than
        # 68 data symbols failed its FCS. 931 B at 24 Mbit/s (a winject bwtest frame)
        # is 78 symbols; 54 Mbit/s with 3000 B reaches the sequence wrap at 127.
        header=bytes.fromhex('08000000 0f0700000000 000000000000 cafebabeb00b f03b')
        for rate,size in ((24,931),(6,400),(54,3000)):
            with self.subTest(rate=rate,size=size):
                mpdu=header+bytes((i*37)&255 for i in range(size-4-len(header)))
                samples,short,long=make_frame(mpdu,rate=rate)
                self.assertIn(rate,self.decode(samples,short,long))

    def test_detector_reports_signal_before_payload_fcs(self):
        mpdu=bytes.fromhex('08000000 ffffffffffff 102030405060 aabbccddeeff 3012')+b'legacy OFDM detector'
        path=pathlib.Path(self.temp.name)/'header.cf32'
        for rate in (6,9,12,18,24,36,48,54):
            with self.subTest(rate=rate):
                samples,short,long=make_frame(mpdu,rate=rate,corrupt_fcs=True)
                np.asarray(samples,np.complex64).tofile(path)
                for exe in (self.exe,self.scalar):
                    r=subprocess.run([exe,path,str(short),str(long)],capture_output=True,text=True,check=True)
                    self.assertIn(f'signal {rate} {len(mpdu)+4}',r.stderr)
                    self.assertNotIn(rate,[int(line.split()[0]) for line in r.stdout.splitlines()])

    def test_signal_is_available_for_truncated_payload(self):
        mpdu=bytes.fromhex('08000000 ffffffffffff 102030405060 aabbccddeeff 3012')+b'long payload '*30
        samples,short,long=make_frame(mpdu,rate=24)
        path=pathlib.Path(self.temp.name)/'truncated.cf32'
        np.asarray(samples[:short+1000],np.complex64).tofile(path)
        for exe in (self.exe,self.scalar):
            r=subprocess.run([exe,path,str(short),str(long)],capture_output=True,text=True,check=True)
            self.assertIn(f'signal 24 {len(mpdu)+4}',r.stderr)
            self.assertEqual(r.stdout,'')

    def test_signal_with_invalid_parity_is_not_reported(self):
        import test_legacy_ofdm as reference
        orig=reference.encode
        def encode_with_bad_signal_parity(bits):
            if len(bits)==24:
                bits=bits.copy();bits[17]^=1
            return orig(bits)
        mpdu=bytes.fromhex('08000000 ffffffffffff 102030405060 aabbccddeeff 3012')+b'payload'
        with patch('test_legacy_ofdm.encode',side_effect=encode_with_bad_signal_parity):
            samples,short,long=make_frame(mpdu,rate=24)
        path=pathlib.Path(self.temp.name)/'bad-signal.cf32'
        np.asarray(samples,np.complex64).tofile(path)
        for exe in (self.exe,self.scalar):
            r=subprocess.run([exe,path,str(short),str(long)],capture_output=True,text=True,check=True)
            self.assertNotIn('signal ',r.stderr)
            self.assertEqual(r.stdout,'')

if __name__=='__main__': unittest.main()
