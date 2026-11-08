"""End-to-end native service smoke test against an isolated UDP radio emulator."""
import asyncio,json,socket,struct,subprocess,threading,time,urllib.request,zlib,sys,math
stop=threading.Event();state={'mode':'idle','job':0,'id':1,'fixture':None,'gain':0,'calibration':False}
def control():
 s=socket.socket(socket.AF_INET,socket.SOCK_DGRAM);s.bind(('127.0.0.1',55022));s.settimeout(.2)
 while not stop.is_set():
  try: d,a=s.recvfrom(2048)
  except TimeoutError:continue
  rid,cmd=d.decode().strip().split(' ',1);rid=rid.split(':')[1]
  if cmd.startswith('iq_info'): result='kind='+state['mode']+' gain_max=2'
  elif cmd.startswith('iq_capture'):state['mode']='capture';state['calibration']='count=1' in cmd;result='capture id=7'
  elif cmd.startswith('iq_sweep'):state.update(mode='sweep',job=11);result='sweep id=11'
  elif cmd.startswith('iq_stop'):state['mode']='idle';result='stop'
  elif cmd.startswith('iq_set gain='):state['gain']=int(cmd.split()[1].split('=')[1]);result='set'
  else:result='set'
  s.sendto(('OK:'+rid+' '+result+'\n').encode(),a)
 s.close()
def capture():
 s=socket.socket(socket.AF_INET,socket.SOCK_DGRAM);s.setsockopt(socket.IPPROTO_IP,socket.IP_MULTICAST_IF,socket.inet_aton('127.0.0.1'));s.setsockopt(socket.IPPROTO_IP,socket.IP_MULTICAST_LOOP,1)
 while not stop.is_set():
  time.sleep(.15)
  if state['mode']=='idle':continue
  fixture=state['fixture']; folder='tests/fixtures/openofdm/' if fixture and fixture.startswith(('ag_', 'dot11a_')) else 'tests/fixtures/libcrafter/'; b=bytearray(open(folder+fixture+'.cs8','rb').read()) if fixture and state['mode']=='capture' else bytearray(i*7%127 for i in range(2048))
  if fixture and state['mode']=='capture':
   for i in range(1,len(b),2):b[i]=(-b[i])%256
  if state['calibration']:
   b=bytearray()
   for i in range(1024):
    a=2*math.pi*80*i/1024
    b.extend((round(24*math.cos(a))%256,(-round(24*math.sin(a)))%256))
  n=len(b)//2;idx=state['id'];state['id']+=1; sweep=state['mode']=='sweep';job=11 if sweep else 7
  for offset in range(0,len(b),1024):
   p=b[offset:offset+1024]; last=offset+len(p)==len(b);flags=(1 if last else 0)|(2|4|16 if sweep and last else 0)
   header=struct.pack('<4sBBBBIIIHHIIIBBHHH',b'WJIQ',2,flags,8,1,idx,n,offset,len(p),44,zlib.crc32(p),2410000,17,state['gain'] if state['calibration'] else 20,0,job,0,1)
   s.sendto(header+p,('239.255.53.1',55303))
  if sweep:state['mode']='idle'
 s.close()
threading.Thread(target=control,daemon=True).start();threading.Thread(target=capture,daemon=True).start()
p=subprocess.Popen([sys.argv[1],'--mplane-port','55022','--host','127.0.0.1','--http-host','127.0.0.1','--http-port','58082','--data-port','55303','--web-dir','web'],stdout=subprocess.PIPE,stderr=subprocess.PIPE)
async def run():
 import websockets
 for _ in range(40):
  try: r=urllib.request.urlopen('http://127.0.0.1:58082/',timeout=.5);assert b'Winject' in r.read();break
  except Exception:await asyncio.sleep(.2)
 else:raise RuntimeError('HTTP unavailable')
 async with websockets.connect('ws://127.0.0.1:58082/ws') as ws:
  async def event(name):
   while True:
    data=json.loads(await asyncio.wait_for(ws.recv(),5))
    if data['event']=='error':raise RuntimeError(str(data))
    if data['event']==name:return data['data']
  st=await event('status');assert st['mode']=='live';print('status',st['decoder_formats'],st['decoder_backend'])
  data=await event('live');assert data['samples']==1024 and len(data['spectrum']['db'])==1024;print('live',data['capture_id'],data['samples'])
  await ws.send(json.dumps({'op':'calibration.start','power_dbm':-60,'tone_hz':2410000000+80*40000000/1024})); done=await event('calibration.done');assert not done['error'] and len(done['points'])==3,done;print('calibration all gains and restoration ok')
  await ws.send(json.dumps({'op':'snapshot'}));snap=await event('snapshot');assert snap['samples']==1024
  assert len(urllib.request.urlopen('http://127.0.0.1:58082/capture/snapshot.cf32').read())==8192;print('snapshot ok')
  state['fixture']='dot11a_24M_40msps_iq8'
  headers={}; payloads={}; common=set()
  while not common:
   msg=await event('frames')
   for frame in msg['frames']:
    key=(frame['capture_id'],frame['start_sample'])
    if frame.get('signal_valid') and frame.get('signal_rate_mbps')==24 and not frame.get('fcs_ok'): headers[key]=frame
    if frame.get('fcs_ok') and frame.get('rate_mbps')==24: payloads[key]=frame
   common=headers.keys() & payloads.keys()
  key=next(iter(common));header=headers[key];payload=payloads[key]
  assert header['signal_length_bytes']==len(payload['mpdu_hex'])//2+4 and header['signal_modulation']=='16-QAM'
  assert not header.get('fcs_ok',False) and payload['fcs_ok']
  assert payload['constellation'] and payload['training_constellation'] and payload['channel_estimate']
  assert payload['frame_spectrum_db'] and payload['frame_envelope_db'] and payload['frame_duration_us'] > 0
  print('OFDM SIGNAL header and independently decoded FCS-verified payload ok')
  state['fixture']='dsss-110-short-clean-48-40msps'
  while True:
   frame=await event('frames')
   good=[f for f in frame['frames'] if f.get('fcs_ok') and f.get('rate_mbps')==11]
   if good:break
  expected=open('tests/fixtures/libcrafter/dsss-110-short-clean-48.mpdu','rb').read()[:-4].hex()
  assert good[0]['mpdu_hex']==expected and 0<=good[0]['start_sample']<5392
  async with websockets.connect('ws://127.0.0.1:58082/ws') as ws2:
   first=json.loads(await asyncio.wait_for(ws2.recv(),5)); second=json.loads(await asyncio.wait_for(ws2.recv(),5))
   assert first['event']=='status' and second['event']=='frames' and second['data']['history']
   assert any(f.get('mpdu_hex')==expected and f.get('fcs_ok') for f in second['data']['frames'])
  print('DSSS FCS-verified frame and reconnect history ok');state['fixture']=None
  await ws.send(json.dumps({'op':'sweep','start_mhz':2410,'stop_mhz':2410,'step_mhz':5,'rate_hz':16000000,'fft':512,'dwell':1}));print('sweep.start',await event('sweep.start'));tile=await event('sweep.tile');assert tile['index']==0 and tile['bins'];print('sweep.tile',len(tile['bins']));print('sweep.done',await event('sweep.done'))
try:asyncio.run(run())
finally:
 stop.set();p.terminate();p.wait(timeout=5);print('server log',p.stderr.read().decode()[-1000:])
