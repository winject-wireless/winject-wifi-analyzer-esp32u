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
    async def test_dsss_worker_exposes_validated_frames(self):
        app=Analyzer(FakeLink());app.mode='live';messages=[]
        async def broadcast(msg): messages.append(msg)
        app.broadcast=broadcast
        class Decoder:
            available=True
            def decode(self, samples, sample_rate_hz, frequency_hz):
                return [{'fcs_ok':True,'start_sample':10,'rate_bps':5500000,'phy':'cck'}]
        app.dsss=Decoder()
        cap=SimpleNamespace(capture_id=7,timestamp_us=12,frequency_hz=2412000000,sample_rate_hz=40000000,job_id=1,samples=lambda:np.zeros(1024,np.complex64))
        task=asyncio.create_task(app.dsss_loop())
        try:
            app.on_capture(cap)
            async with asyncio.timeout(1):
                while not app.dsss_decoded: await asyncio.sleep(.01)
            frame=app.decoded_records[-1]
            self.assertEqual(frame['rate_mbps'],5.5)
            self.assertEqual(frame['status'],'FCS verified')
            self.assertEqual(frame['capture_id'],7)
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
        cap=SimpleNamespace(capture_id=7,timestamp_us=12,frequency_hz=2412000000,sample_rate_hz=40000000,job_id=1,samples=lambda:np.zeros(1024,np.complex64))
        with patch('winject_wifi_analyzer.frame_detector.detect_frames',return_value=[candidate]), patch('winject_wifi_analyzer.legacy_ofdm.decode_legacy_ofdm',return_value=decoded):
            tasks=[asyncio.create_task(app.frame_loop()),asyncio.create_task(app.decode_loop())]
            try:
                app.on_capture(cap)
                async with asyncio.timeout(1):
                    while app.decoder_decoded==0:await asyncio.sleep(.01)
                frame=app.frame_records[-1]
                self.assertEqual(frame['status'],'FCS verified');self.assertEqual(frame['addr1'],'aa:bb:cc:dd:ee:ff')
                self.assertEqual(frame['timestamp_us'],12)
                self.assertEqual(sum(m['event']=='frames' for m in messages),2)
            finally:
                for task in tasks: task.cancel()
                await asyncio.gather(*tasks,return_exceptions=True)
