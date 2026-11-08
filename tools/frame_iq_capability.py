#!/usr/bin/env python3
"""Passive channel-1 Wi-Fi callback vs RF dump capability check."""
import socket,struct,threading,time
HOST='192.168.253.14'; LOCAL='192.168.253.106'; MON='wlx00c0cabce072'; DURATION=12
ctl=socket.socket(socket.AF_INET,socket.SOCK_DGRAM);ctl.bind((LOCAL,0));ctl.settimeout(3)
rid=0
def request(port,line):
 global rid
 if port==22:
  rid=(rid%255)+1; line=f'cmd:{rid} {line}'
 ctl.sendto(line.encode(),(HOST,port))
 while True:
  b,peer=ctl.recvfrom(4096)
  if peer==(HOST,port):
   text=b.decode().strip()
   if port==22 and not all(l.startswith(f'OK:{rid} ') or l==f'OK:{rid}' for l in text.splitlines()): raise RuntimeError(text)
   return text
def frames():
 s=request(5500,'FRAMES'); return {k:int(v) for k,v in (x.split('=') for x in s.split()[1:])}
def info(): return request(22,'iq_info')
mon=socket.socket(socket.AF_PACKET,socket.SOCK_RAW,socket.htons(3));mon.bind((MON,0));mon.settimeout(.1);mon.setsockopt(socket.SOL_SOCKET,socket.SO_RCVBUF,4*1024*1024)
samples=socket.socket(socket.AF_INET,socket.SOCK_DGRAM);samples.setsockopt(socket.SOL_SOCKET,socket.SO_RCVBUF,4*1024*1024);samples.bind((LOCAL,5300));samples.settimeout(.1)
lock=threading.Lock(); counts={'mon_total':0,'mon_mgmt':0,'mon_ctrl':0,'mon_data':0,'mon_other':0,'sample_packets':0,'sample_bytes':0}; stop=threading.Event()
def observe():
 while not stop.is_set():
  try: b=mon.recv(8192)
  except socket.timeout: continue
  if len(b)<10: continue
  h=int.from_bytes(b[2:4],'little')
  if h+2>len(b): continue
  t=(b[h]>>2)&3;key={0:'mon_mgmt',1:'mon_ctrl',2:'mon_data'}.get(t,'mon_other')
  with lock:counts['mon_total']+=1;counts[key]+=1
def receive():
 while not stop.is_set():
  try:b,p=samples.recvfrom(2048)
  except socket.timeout:continue
  if p[0]==HOST and b[:4]==b'WJIQ':
   with lock: counts['sample_packets']+=1;counts['sample_bytes']+=len(b)
t1=threading.Thread(target=observe);t2=threading.Thread(target=receive);t1.start();t2.start()
try:
 print('monitor interface',MON,'channel 1 2412 MHz; passive observations',flush=True)
 print('set',request(22,'iq_stop'),request(22,'iq_set freq=2412 gain=agc bw=0'),flush=True)
 for phase,interval in [('idle_before',None),('slow_200ms',200),('fast',0),('idle_after',None)]:
  if interval is None: print('phase_setup',phase,request(22,'iq_stop'),flush=True)
  else: print('phase_setup',phase,request(22,f'iq_capture n=16380 rate=16000000 bits=8 count=0 interval_ms={interval} dst={LOCAL}:5300'),flush=True)
  time.sleep(1.5)
  i0=info(); a=frames()
  with lock: h0=counts.copy()
  start=time.monotonic();time.sleep(DURATION);elapsed=time.monotonic()-start
  b=frames();i1=info()
  with lock: h1=counts.copy()
  delta={k:b[k]-a[k] for k in a if k not in ('last_wifi_ts_us','uptime_us')}
  host_delta={k:h1[k]-h0[k] for k in h0}
  print('result',phase,'seconds',round(elapsed,3),'esp',delta,'monitor',host_delta,'esp_uptime_delta_us',b['uptime_us']-a['uptime_us'],flush=True)
  print('iq_before',i0.replace('\n',' | '),'iq_after',i1.replace('\n',' | '),flush=True)
finally:
 try: print('cleanup',request(22,'iq_stop'),flush=True)
 except Exception as e: print('cleanup_failed',str(e),flush=True)
 stop.set();t1.join();t2.join();mon.close();samples.close();ctl.close()
