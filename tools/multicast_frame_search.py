#!/usr/bin/env python3
"""Experimental WJIQ + WJEV multicast recorder and Wi-Fi preamble candidate search."""
import argparse,bisect,collections,json,socket,struct,time,zlib
import numpy as np
from pathlib import Path

p=argparse.ArgumentParser();p.add_argument('--seconds',type=float,default=20);p.add_argument('--output',default='../multicast-frame-experiment');a=p.parse_args()
HOST='192.168.253.14'; LOCAL='192.168.253.106'; GROUP='239.255.53.1'; PORT=5300
OUT=Path(a.output);OUT.mkdir(exist_ok=True,parents=True)
ctl=socket.socket(socket.AF_INET,socket.SOCK_DGRAM);ctl.bind((LOCAL,0));ctl.settimeout(3); rid=0
def request(port,line):
 global rid
 if port==22:rid=rid%255+1;line=f'cmd:{rid} {line}'
 ctl.sendto(line.encode(),(HOST,port))
 while True:
  d,peer=ctl.recvfrom(4096)
  if peer==(HOST,port):
   s=d.decode().strip()
   if port==22 and not all(l.startswith(f'OK:{rid} ') or l==f'OK:{rid}' for l in s.splitlines()):raise RuntimeError(s)
   return s
s=socket.socket(socket.AF_INET,socket.SOCK_DGRAM,socket.IPPROTO_UDP);s.setsockopt(socket.SOL_SOCKET,socket.SO_REUSEADDR,1);s.setsockopt(socket.SOL_SOCKET,socket.SO_RCVBUF,8*1024*1024);s.bind(('',PORT));s.setsockopt(socket.IPPROTO_IP,socket.IP_ADD_MEMBERSHIP,socket.inet_aton(GROUP)+socket.inet_aton(LOCAL));s.settimeout(.3)
H=struct.Struct('<4sBBBBIIIHHIIIBBHHH');E=struct.Struct('<4sIQHbB24s')
open_caps={};complete=[]; events=[]; stats=collections.Counter(); t0=time.monotonic()
print('listening',GROUP,PORT,'SO_RCVBUF',s.getsockopt(socket.SOL_SOCKET,socket.SO_RCVBUF),flush=True)
try:
 print('events_before',request(5500,'EVENTS'),flush=True)
 print(request(22,'iq_stop'),flush=True)
 print(request(22,'iq_set freq=2412 gain=agc bw=0'),flush=True)
 print(request(22,f'iq_capture n=16380 rate=40000000 bits=8 count=0 interval_ms=0 dst={GROUP}:{PORT}'),flush=True)
 end=time.monotonic()+a.seconds
 while time.monotonic()<end:
  try:d,peer=s.recvfrom(2048)
  except socket.timeout:continue
  if peer[0]!=HOST:continue
  if d[:4]==b'WJEV' and len(d)==E.size:
   _,wifi,cb,sz,rssi,ch,frame=E.unpack(d);events.append((wifi,cb,sz,rssi,ch,frame.hex()));stats['events']+=1;continue
  if d[:4]!=b'WJIQ' or len(d)<H.size:stats['other']+=1;continue
  f=H.unpack_from(d);_,ver,flags,bits,rate,cid,n,off,plen,hlen,crc,freq,ts,gain,bw,job,step,steps=f
  data=d[hlen:]
  if ver!=2 or bits!=8 or n!=16380 or rate!=1 or hlen!=44 or len(data)!=plen or off+plen>n*2 or zlib.crc32(data)&0xffffffff!=crc:
   stats['invalid']+=1;continue
  stats['sample_datagrams']+=1
  if cid not in open_caps:open_caps[cid]=[ts,bytearray(n*2),set(),0]
  c=open_caps[cid]
  if off not in c[2]:c[1][off:off+plen]=data;c[2].add(off);c[3]+=plen
  if c[3]==n*2:
   complete.append((ts,cid,bytes(c[1])));del open_caps[cid];stats['complete']+=1
  if len(open_caps)>150:
   k=next(iter(open_caps));del open_caps[k];stats['evicted_incomplete']+=1
finally:
 try:print('stop',request(22,'iq_stop'),flush=True);print('events_after',request(5500,'EVENTS'),flush=True);print('info',request(22,'iq_info').replace('\n',' | '),flush=True)
 except Exception as ex:print('control_error',ex,flush=True)
 s.close();ctl.close()
print('received',dict(stats),'incomplete',len(open_caps),flush=True)
if not events or not complete:raise SystemExit('Need both event and complete IQ packets; no candidate can be examined.')
# Wi-Fi clock epoch is unknown. Minimum callback - RX timestamp is an upper
# estimate of the epoch offset if the Wi-Fi timestamp precedes its callback.
# It includes unmeasured callback latency and unknown hardware timestamp point.
mask=0xffffffff
def signed32(v):return ((v+0x80000000)&mask)-0x80000000
offsets=np.array([((cb&mask)-wifi)&mask for wifi,cb,_,_,_,_ in events],dtype=np.uint64)
offset=int(np.min(offsets));print('wifi_to_esp_offset_est_us',offset,'callback_minus_wifi range',int(offsets.min()),int(np.median(offsets)),int(offsets.max()),flush=True)
# Relative burst times referenced to the first complete capture (wrap safe).
base=complete[0][0];times=[signed32(c[0]-base) for c in complete]
order=np.argsort(times);times=[times[i] for i in order];complete=[complete[i] for i in order]

def score(payload):
 x=np.frombuffer(payload,dtype=np.int8).astype(np.float32).reshape(-1,2)
 z=x[:,0]-1j*x[:,1];lag=32;w=256
 a=z[:-lag];b=z[lag:];p=a*np.conj(b);aa=np.abs(a)**2;bb=np.abs(b)**2
 def rolling(v):
  c=np.concatenate(([0],np.cumsum(v,dtype=np.complex128 if np.iscomplexobj(v) else np.float64)));return c[w:]-c[:-w]
 pp=rolling(p);ea=rolling(aa);eb=rolling(bb);metric=np.abs(pp)**2/(ea*eb+1e-9)
 # Ignore very weak regions, which can produce unstable high scores.
 valid=(ea/w>16)&(eb/w>16)
 if not np.any(valid):return 0.,0,0.
 metric[~valid]=0;idx=int(np.argmax(metric));return float(metric[idx]),idx,float(ea[idx]/w)

# For each event scan complete bursts within +/- 1 ms of the estimated Wi-Fi
# timestamp. Search scores alone cannot identify a particular frame.
records=[];cache={};event_matches=0
for j,(wifi,cb,sz,rssi,ch,frame) in enumerate(events):
 et=signed32(((wifi+offset)&mask)-base);lo=bisect.bisect_left(times,et-1500);hi=bisect.bisect_right(times,et+1000)
 if lo==hi:continue
 event_matches+=1
 for k in range(lo,hi):
  if k not in cache:cache[k]=score(complete[k][2])
  q,idx,power=cache[k];delta_us=(times[k]+idx/40)-et
  records.append({'score':q,'capture_id':complete[k][1],'capture_start_us':complete[k][0],'sample_index':idx,'energy':round(power,2),'event_index':j,'event_wifi_us':wifi,'event_callback_us':cb,'event_relative_us':et,'candidate_minus_est_event_us':round(delta_us,2),'rssi_dbm':rssi,'channel':ch,'frame_length':sz})
records.sort(key=lambda x:x['score'],reverse=True)
print('events_with_nearby_complete_capture',event_matches,'candidate_comparisons',len(records),'unique_scanned_bursts',len(cache),flush=True)
print('top_candidates',json.dumps(records[:8],indent=2),flush=True)
(OUT/'events.json').write_text(json.dumps([{'wifi_us':w,'callback_us':c,'length':l,'rssi':r,'channel':ch,'header':h} for w,c,l,r,ch,h in events],indent=2))
(OUT/'candidates.json').write_text(json.dumps({'offset_estimate_us':offset,'stats':dict(stats),'incomplete':len(open_caps),'candidates':records[:100]},indent=2))
for i,item in enumerate(records[:5]):
 entry=next(c for c in complete if c[1]==item['capture_id']);raw=np.frombuffer(entry[2],np.int8).reshape(-1,2).astype(np.float32); z=(raw[:,0]-1j*raw[:,1])/127.75
 z.astype('<c8').tofile(OUT/f'candidate-{i+1}-capture-{entry[1]}.cf32')
print('saved',OUT,'sample_rate=40000000 complex64 little-endian; scores are periodicity candidates, not decoded-frame confirmation',flush=True)
