"""Legacy OFDM PHY independent vectors and synthetic round trips."""
import math
from pathlib import Path
from unittest.mock import patch
import unittest
import zlib
import numpy as np
from scipy.signal import resample_poly
from winject_wifi_analyzer.legacy_ofdm import decode_legacy_6mbps, _LTF_BINS, _POLARITY
from winject_wifi_analyzer.frame_detector import detect_frames

DATA = [k for k in range(-26,27) if k and k not in (-21,-7,7,21)]
PILOTS = [-21,-7,7,21]

def encode(bits):
    state=0;out=[]
    for bit in bits:
        state=((state<<1)|int(bit))&0x7f
        out.extend(((state&0o155).bit_count()%2,(state&0o117).bit_count()%2))
    return np.array(out,np.uint8)

RATE_TABLE = {
    6: ('1101',1,24,(1,1)), 9: ('1111',1,36,(1,1,1,0,0,1)),
    12: ('0101',2,48,(1,1)), 18: ('0111',2,72,(1,1,1,0,0,1)),
    24: ('1001',4,96,(1,1)), 36: ('1011',4,144,(1,1,1,0,0,1)),
    48: ('0001',6,192,(1,1,1,0)), 54: ('0011',6,216,(1,1,1,0,0,1)),
}

def symbol(bits, pilot, bpsc=1):
    count=48*bpsc; interleaved=np.zeros(count,np.uint8);spacing=max(1,bpsc//2)
    for k in range(count):
        i=(count//16)*(k%16)+k//16
        j=spacing*(i//spacing)+(i+count-(16*i)//count)%spacing
        interleaved[j]=bits[k]
    bins=np.zeros(64,np.complex128)
    points=[]
    for chunk in interleaved.reshape(48,bpsc):
        if bpsc==1:points.append(complex(2*int(chunk[0])-1,0));continue
        half=bpsc//2
        mapping={1:(-1,1),2:(-3,-1,3,1),3:(-7,-5,-1,-3,7,5,1,3)}[half]
        i=sum(int(b)<<j for j,b in enumerate(chunk[:half][::-1]))
        q=sum(int(b)<<j for j,b in enumerate(chunk[half:][::-1]))
        points.append(complex(mapping[i],mapping[q])/math.sqrt({2:2,4:10,6:42}[bpsc]))
    bins[np.array(DATA)%64]=points
    bins[np.array(PILOTS)%64]=pilot*np.array([1,1,1,-1])
    t=np.fft.ifft(bins);return np.r_[t[-16:],t]

def make_frame(payload, corrupt_fcs=False, rate=6):
    code,bpsc,ndbps,pattern=RATE_TABLE[rate]
    fcs = (zlib.crc32(payload)&0xffffffff) ^ int(corrupt_fcs)
    frame=payload+fcs.to_bytes(4,'little')
    signal=np.zeros(24,np.uint8);signal[:4]=[int(bit) for bit in code]
    for i in range(12):signal[5+i]=(len(frame)>>i)&1
    signal[17]=sum(signal[:17])%2
    n=math.ceil((16+8*len(frame)+6)/ndbps)
    source=np.zeros(n*ndbps,np.uint8);source[16:16+len(frame)*8]=np.unpackbits(np.frombuffer(frame,np.uint8),bitorder='little')
    scrambler=[1,0,1,1,1,0,1];scrambled=[]
    for bit in source:
        f=scrambler[6]^scrambler[3];scrambled.append(int(bit)^f);scrambler=[f]+scrambler[:-1]
    tail_start=16+8*len(frame);scrambled[tail_start:tail_start+6]=[0]*6
    mother=encode(scrambled)
    coded=mother[np.resize(np.asarray(pattern,dtype=bool),len(mother))]
    short16=np.fft.ifft(np.array([0,0,1+1j,0,0,0,-1-1j,0,0,0,1+1j,0,0,0,-1-1j,0]))
    ltf=np.fft.ifft(_LTF_BINS)
    iq20=np.r_[np.zeros(60), np.tile(short16,10), ltf[-32:], ltf, ltf,
               symbol(encode(signal),_POLARITY[0]),
               *[symbol(coded[48*bpsc*i:48*bpsc*(i+1)],_POLARITY[(i+1)%127],bpsc) for i in range(n)],
               np.zeros(100)]
    iq40=resample_poly(iq20,2,1).astype(np.complex64)
    iq40*=np.exp(2j*np.pi*7000*np.arange(len(iq40))/40e6)
    return iq40, (60+80)*2, (60+160+32)*2

class LegacyOFDMTest(unittest.TestCase):
    def test_pre_pilot_data_preserves_payload_phase(self):
        from winject_wifi_analyzer import legacy_ofdm
        mpdu=bytes.fromhex('08000000 ffffffffffff 102030405060 aabbccddeeff 3012')+b'phase test'
        original = symbol
        calls = 0
        def rotated(*args, **kwargs):
            nonlocal calls
            calls += 1
            out = original(*args, **kwargs)
            return out if calls == 1 else out * np.exp(0.4j)
        with patch(__name__ + '.symbol', side_effect=rotated):
            samples, short, long = make_frame(mpdu, rate=24)
        result=legacy_ofdm.decode_legacy_ofdm(samples,short,long)
        self.assertIsNotNone(result)
        raw=np.array([complex(*p) for p in result['raw_data_iq']])
        corrected=np.array([complex(*p) for p in result['constellation']])
        phase=np.angle(np.vdot(corrected, raw))
        self.assertAlmostEqual(phase, 0.4, delta=0.03)
        self.assertEqual(len(result['raw_training_iq']), 104)
        self.assertLess(np.mean(np.abs(np.abs(np.array([complex(*p) for p in result['raw_training_iq']]))-1)), 0.1)

    def test_independent_openofdm_vector(self):
        for rate in (6, 54):
            with self.subTest(rate=rate):
                self.check_independent_openofdm_vector(rate)

    def check_independent_openofdm_vector(self, rate):
        vector = Path(__file__).parent / f'fixtures/openofdm/ag_{rate}M_len14_pre100_post200_openwifi.txt'
        iq = np.loadtxt(vector)
        samples = resample_poly(iq[:, 0] + 1j * iq[:, 1], 2, 1).astype(np.complex64) / 32768
        seen = []
        def crc_checked(psdu):
            seen.append(psdu)
            if len(psdu) < 4 or zlib.crc32(psdu[:-4]) & 0xffffffff != int.from_bytes(psdu[-4:], 'little'):
                return None
            return {'mpdu_hex': psdu[:-4].hex()}
        with patch('winject_wifi_analyzer.legacy_ofdm.parse_mpdu', side_effect=crc_checked):
            result = decode_legacy_6mbps(samples, 360, 584)
        self.assertIsNotNone(result)
        self.assertEqual(result['mpdu_hex'], '00010203040506070809')
        self.assertEqual(result['rate_mbps'], rate)
        self.assertEqual(seen[0][-4:].hex(), '46d76c45')

    def test_independent_conducted_openofdm_vectors(self):
        root=Path(__file__).parent / 'fixtures/openofdm'
        for rate in (9,12,18,24,36,48):
            with self.subTest(rate=rate):
                vector=next(root.glob(f'dot11a_{rate}mbps_*.dat'))
                raw=np.fromfile(vector,dtype='<i2').astype(np.float32).reshape(-1,2)
                samples=resample_poly(raw[:,0]+1j*raw[:,1],2,1).astype(np.complex64)/32768
                found=[]
                for c in detect_frames(samples,40_000_000):
                    r=decode_legacy_6mbps(samples,c['start_sample'],c['long_start_sample'])
                    if r and r['rate_mbps']==rate:
                        found.append(r)
                        break
                self.assertTrue(found,'no FCS-valid frame at expected rate')
                self.assertTrue(found[0]['fcs_ok'])
                self.assertTrue(found[0]['mpdu_hex'])

    def test_pilot_polarity_is_the_80211_scrambler_sequence(self):
        # IEEE 802.11 17.3.5.10: p_n = 1 - 2 * scrambler output for the all-ones seed.
        # The generator below uses this table too, so the round trips cannot catch a typo.
        state, expected = 0x7f, []
        for _ in range(127):
            bit = ((state >> 6) ^ (state >> 3)) & 1
            expected.append(1 - 2 * bit)
            state = ((state << 1) | bit) & 0x7f
        self.assertEqual(_POLARITY.tolist(), expected)
        native = (Path(__file__).resolve().parents[1] / 'native/ofdm.cpp').read_text()
        table = native.split('polarity[] = {', 1)[1].split('}', 1)[0]
        self.assertEqual([int(v) for v in table.replace('\n', ' ').split(',') if v.strip()], expected)

    def test_synthetic_roundtrip_all_rates(self):
        mpdu=bytes.fromhex('08000000 ffffffffffff 102030405060 aabbccddeeff 3012')+b'legacy OFDM'
        expected={6:'BPSK',9:'BPSK',12:'QPSK',18:'QPSK',24:'16-QAM',36:'16-QAM',48:'64-QAM',54:'64-QAM'}
        for rate,modulation in expected.items():
            with self.subTest(rate=rate):
                x,short,long=make_frame(mpdu,rate=rate)
                result=decode_legacy_6mbps(x,short,long)
                self.assertIsNotNone(result)
                self.assertEqual(result['mpdu_hex'],mpdu.hex())
                self.assertEqual(result['rate_mbps'],rate)
                self.assertEqual(result['modulation'],modulation)
                self.assertIsNone(decode_legacy_6mbps(x[:-300],short,long))
                bad,short,long=make_frame(mpdu,rate=rate,corrupt_fcs=True)
                self.assertIsNone(decode_legacy_6mbps(bad,short,long))

    def test_near_rail_iq_withholds_evm_but_not_valid_frame(self):
        mpdu=bytes.fromhex('08000000 ffffffffffff 102030405060 aabbccddeeff 3012')+b'hello'
        x,short,long=make_frame(mpdu)
        result=decode_legacy_6mbps(x*5,short,long)
        self.assertIsNotNone(result)
        self.assertTrue(result['fcs_ok'])
        self.assertGreater(result['payload_clipped_fraction'], .005)
        self.assertIsNone(result['evm_percent'])
        self.assertIn('clipped',result['evm_warning'])

    def test_synthetic_frame_fcs_and_truncation(self):
        mpdu=bytes.fromhex('08000000 ffffffffffff 102030405060 aabbccddeeff 3012')+b'hello'
        x,short,long=make_frame(mpdu)
        result=decode_legacy_6mbps(x,short,long)
        self.assertIsNotNone(result)
        self.assertEqual(result['mpdu_hex'],mpdu.hex())
        self.assertEqual(result['addr3'],'aa:bb:cc:dd:ee:ff')
        self.assertEqual(result['modulation'],'BPSK')
        self.assertIsNone(decode_legacy_6mbps(x[:-300],short,long))
        bad,short,long=make_frame(mpdu,corrupt_fcs=True)
        self.assertIsNone(decode_legacy_6mbps(bad,short,long))

if __name__=='__main__':unittest.main()
