"""Integration checks for host recovery and candidate-to-decoded-frame handoff."""
import asyncio
import unittest
from types import SimpleNamespace
from unittest.mock import patch
import numpy as np
from winject_wifi_analyzer.server import Analyzer

class FakeLink:
    def __init__(self):
        self.requests=[];self.radio_ip='192.0.2.1';self.dst='239.255.53.1:5300'
        self.datagrams=0;self.bytes=0
        self.assembler=SimpleNamespace(completed=0,lost=0,errors=0)
        self.on_capture=None
    async def info(self): return {'kind':'idle'}
    async def request(self,cmd): self.requests.append(cmd);return []

class ServerPipelineTest(unittest.IsolatedAsyncioTestCase):
    async def test_gain_calibration_tracks_jobs_skips_clipping_and_restores(self):
        class CalibrationLink(FakeLink):
            async def info(self): return {'gain_max':'1'}
            async def request(self, cmd):
                self.requests.append(cmd)
                if cmd.startswith('iq_set gain='):
                    self.gain = int(cmd.split('=')[1])
                if cmd.startswith('iq_capture') and 'count=9' in cmd:
                    gain = self.gain
                    n = 1024
                    amplitude = .15 if gain == 0 else 1.5
                    samples = (amplitude * np.exp(2j*np.pi*80*np.arange(n)/n)).astype(np.complex64)
                    for i in range(9):
                        cap = SimpleNamespace(capture_id=1000+gain*9+i, job_id=100+gain, gain_index=gain, agc=False, samples=lambda:samples, sample_rate_hz=40000000, frequency_hz=2412000000, sample_count=n, bits=8)
                        asyncio.get_running_loop().call_soon(self.on_capture, cap)
                    return [f'iq_capture id={100+gain}']
                return []
        link=CalibrationLink(); app=Analyzer(link); app.mode='live'; app.config.update(n=1024, nfft=1024, freq_mhz=2412, gain='agc')
        messages=[]
        async def broadcast(msg): messages.append(msg)
        app.broadcast=broadcast
        await app.calibrate_gains(-40, 2412000000 + 79*40000000/1024)
        done=next(m['data'] for m in messages if m['event']=='calibration.done')
        self.assertIsNone(done['error'])
        self.assertEqual([p['settings']['gain'] for p in done['points']], [0])
        self.assertEqual(done['skipped'][0]['clipped'],9)
        self.assertEqual(done['points'][0]['inliers'],9)
        self.assertAlmostEqual(done['points'][0]['measured_dbfs'], 20*np.log10(.15), places=1)
        self.assertTrue(any('gain=agc' in cmd for cmd in link.requests))
        self.assertEqual(app.mode, 'live')

    async def test_cancelled_gain_calibration_restores_idle_radio(self):
        class CalibrationLink(FakeLink):
            async def info(self): return {'gain_max':'1'}
            async def request(self, cmd):
                self.requests.append(cmd)
                if cmd.startswith('iq_capture'): return ['iq_capture id=1']
                return []
        link=CalibrationLink(); app=Analyzer(link); messages=[]
        async def broadcast(msg): messages.append(msg)
        app.broadcast=broadcast
        task=asyncio.create_task(app.calibrate_gains(-40, 2440000000))
        while app.mode != 'calibration': await asyncio.sleep(0)
        task.cancel(); await task
        self.assertEqual(app.mode, 'idle')
        self.assertTrue(any('gain=agc' in cmd for cmd in link.requests))
        done=next(m['data'] for m in messages if m['event']=='calibration.done')
        self.assertTrue(done['cancelled']); self.assertEqual(done['points'], [])

    async def test_dsss_worker_exposes_validated_frames(self):
        app=Analyzer(FakeLink());app.mode='live';messages=[]
        async def broadcast(msg): messages.append(msg)
        app.broadcast=broadcast
        class Decoder:
            available=True
            def decode(self, samples, sample_rate_hz, frequency_hz):
                return [{'fcs_ok':True,'start_sample':10,'rate_bps':5500000,'phy':'cck'}]
        app.dsss=Decoder()
        cap=SimpleNamespace(capture_id=7,timestamp_us=12,frequency_hz=2412000000,sample_rate_hz=40000000,gain_index=37,agc=True,sample_count=1024,bandwidth_mhz=0,bits=8,job_id=1,samples=lambda:np.zeros(1024,np.complex64))
        task=asyncio.create_task(app.dsss_loop())
        try:
            app.on_capture(cap)
            async with asyncio.timeout(1):
                while not app.dsss_decoded: await asyncio.sleep(.01)
            frame=app.decoded_records[-1]
            self.assertEqual(frame['rate_mbps'],5.5)
            self.assertEqual(frame['status'],'FCS verified')
            self.assertEqual(frame['capture_id'],7)
            self.assertEqual((frame['gain_index'], frame['agc']), (37, True))
            self.assertEqual(messages[-1]['data']['frames'],[frame])
        finally:
            task.cancel();await asyncio.gather(task,return_exceptions=True)

    async def test_recovers_requested_live_stream_when_radio_returns_idle(self):
        link=FakeLink();app=Analyzer(link,ui_hz=100)
        task=asyncio.create_task(app.ui_loop())
        try:
            async with asyncio.timeout(1):
                while app.mode!='live': await asyncio.sleep(.01)
            self.assertTrue(any(x.startswith('iq_set ') for x in link.requests))
            self.assertTrue(any('dst=239.255.53.1:5300' in x for x in link.requests))
        finally:
            task.cancel();await asyncio.gather(task,return_exceptions=True)

    async def test_history_retains_verified_frame_after_candidates_rotate(self):
        link=FakeLink();app=Analyzer(link)
        verified={'capture_id':1,'start_sample':2,'fcs_ok':True,'mpdu_hex':'0800'}
        app.decoded_records.append(verified)
        for i in range(300):
            app.frame_records.append({'capture_id':i+10,'start_sample':0})
        class FakeWebSocket:
            def __init__(self): self.sent=[]
            async def send(self, text): self.sent.append(__import__('json').loads(text))
            def __aiter__(self):
                async def empty():
                    if False: yield None
                return empty()
        ws=FakeWebSocket();await app.handle(ws,'/ws')
        history=next(m['data'] for m in ws.sent if m['event']=='frames')
        self.assertTrue(history['history'])
        self.assertIn(verified,history['frames'])
        self.assertEqual(len(history['frames']),201)

    async def test_candidate_is_updated_only_after_decoder_success(self):
        link=FakeLink();app=Analyzer(link);app.mode='live';messages=[]
        async def broadcast(msg):messages.append(msg)
        app.broadcast=broadcast
        candidate={'start_sample':50,'long_start_sample':250,'short_score':.99,'long_score':.99,'status':'candidate','modulation':None,'mpdu_hex':None}
        decoded={'fcs_ok':True,'modulation':'BPSK','mpdu_hex':'0800','addr1':'aa:bb:cc:dd:ee:ff'}
        cap=SimpleNamespace(capture_id=7,timestamp_us=12,frequency_hz=2412000000,sample_rate_hz=40000000,gain_index=37,agc=True,sample_count=1024,bandwidth_mhz=0,bits=8,job_id=1,samples=lambda:np.zeros(1024,np.complex64))
        with patch('winject_wifi_analyzer.frame_detector.detect_frames',return_value=[candidate]), patch('winject_wifi_analyzer.legacy_ofdm.decode_legacy_ofdm',return_value=decoded):
            tasks=[asyncio.create_task(app.frame_loop()),asyncio.create_task(app.decode_loop())]
            try:
                app.on_capture(cap)
                async with asyncio.timeout(1):
                    while app.decoder_decoded==0:await asyncio.sleep(.01)
                frame=app.frame_records[-1]
                self.assertEqual(frame['status'],'FCS verified');self.assertEqual(frame['addr1'],'aa:bb:cc:dd:ee:ff')
                self.assertEqual(frame['timestamp_us'],12)
                self.assertEqual((frame['gain_index'], frame['agc']), (37, True))
                self.assertEqual(sum(m['event']=='frames' for m in messages),2)
            finally:
                for task in tasks: task.cancel()
                await asyncio.gather(*tasks,return_exceptions=True)

class FrameWaveformTest(unittest.TestCase):
    def test_complete_interval_and_power(self):
        from winject_wifi_analyzer.server import frame_waveform
        import numpy as np
        samples = np.r_[np.zeros(10), np.ones(100), np.zeros(10)].astype(np.complex64)
        result = frame_waveform(samples, 10, 110, 1_000_000)
        self.assertEqual(result['frame_duration_us'], 100)
        self.assertEqual(result['frame_peak_dbfs'], 0)
        self.assertEqual(result['frame_mean_dbfs'], 0)
        self.assertEqual(len(result['frame_envelope_db']), 100)
        self.assertEqual(frame_waveform(samples, -1, 110, 1_000_000), {})
        self.assertEqual(frame_waveform(samples, 10, 121, 1_000_000), {})
        x = (0.1 + 0.2*np.exp(2j*np.pi*np.arange(100)/10)).astype(np.complex64)
        measured = frame_waveform(x, 0, len(x), 1_000_000)
        self.assertAlmostEqual(measured['frame_rssi_dbfs'], 20*np.log10(.2), places=1)
        self.assertFalse(measured['frame_clipped'])
        # A bin-centered tone has the same dBFS reference as calibration.
        tone = (0.2*np.exp(2j*np.pi*30*np.arange(2048)/512)).astype(np.complex64)
        spectrum = frame_waveform(tone, 0, len(tone), 1_000_000)['frame_spectrum_db']
        self.assertAlmostEqual(max(spectrum), 20*np.log10(.2), places=1)
