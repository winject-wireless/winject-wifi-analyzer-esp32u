#!/usr/bin/env python3
"""Experimental streaming 802.11 OFDM preamble candidate detector.

40 MS/s WJIQ IQ8 multicast; matches short and long periodic regions. A
candidate is not a decoded frame. Counters expose network and processing loss.
"""
import argparse, collections, json, queue, socket, struct, threading, time, zlib
import numpy as np

H=struct.Struct('<4sBBBBIIIHHIIIBBHHH'); GROUP='239.255.53.1';PORT=5300

def moving(v,n):
    s=np.empty(len(v)+1,dtype=np.complex128 if np.iscomplexobj(v) else np.float64);s[0]=0;np.cumsum(v,out=s[1:]);return s[n:]-s[:-n]

def similarity(x,lag,w):
    a=x[:-lag];b=x[lag:];cross=moving(a*np.conj(b),w);ea=moving(np.abs(a)**2,w);eb=moving(np.abs(b)**2,w)
    return np.abs(cross)**2/(ea*eb+1e-9),ea/w

def detect(x):
    # 40 MS/s: 802.11 legacy short periodicity 32 samples, long 128.
    q32,power=similarity(x,32,128);q128,_=similarity(x,128,128)
    mask=(q32>.82)&(power>16.0);edges=np.diff(np.r_[False,mask,False].astype(np.int8));starts=np.flatnonzero(edges==1);ends=np.flatnonzero(edges==-1)
    found=[]
    for a,b in zip(starts,ends):
        if b-a<24:continue
        st=int(a+np.argmax(q32[a:b]));lo=st+80;hi=min(st+600,len(q128))
        if lo>=hi:continue
        lt=int(lo+np.argmax(q128[lo:hi]));long=float(q128[lt])
        if long<.82:continue
        found.append((st,lt,float(q32[st]),long,float(power[st])))
    return found

def to_complex(payload):
    q=np.frombuffer(payload,np.int8).astype(np.float32).reshape(-1,2)
    return q[:,0]-1j*q[:,1]

def run_offline(path):
    x=np.fromfile(path,dtype='<c8')*127.75
    results=detect(x);print(json.dumps({'file':path,'samples':len(x),'detections':results}));return bool(results)

def live(args):
    h=args.host;local=args.interface_ip
    ctl=socket.socket(socket.AF_INET,socket.SOCK_DGRAM);ctl.bind((local,0));ctl.settimeout(3);rid=0
    def request(port,line):
        nonlocal rid
        if port==22:rid=rid%255+1;line=f'cmd:{rid} {line}'
        ctl.sendto(line.encode(),(h,port))
        while True:
            data,peer=ctl.recvfrom(4096)
            if peer==(h,port):
                s=data.decode().strip()
                if port==22 and not all(l.startswith(f'OK:{rid} ') or l==f'OK:{rid}' for l in s.splitlines()):raise RuntimeError(s)
                return s
    s=socket.socket(socket.AF_INET,socket.SOCK_DGRAM);s.setsockopt(socket.SOL_SOCKET,socket.SO_REUSEADDR,1);s.setsockopt(socket.SOL_SOCKET,socket.SO_RCVBUF,8*1024*1024);s.bind(('',PORT));s.setsockopt(socket.IPPROTO_IP,socket.IP_ADD_MEMBERSHIP,socket.inet_aton(GROUP)+socket.inet_aton(local));s.settimeout(.2)
    work=queue.Queue(maxsize=32);lock=threading.Lock();stat=collections.Counter();processing_ms=[];latency_ms=[];open_caps={}
    def worker():
        while True:
            item=work.get()
            if item is None:work.task_done();return
            cid,ts,payload,enqueued=item;started=time.monotonic();res=detect(to_complex(payload));ended=time.monotonic()
            with lock:
                stat['processed']+=1;stat['candidate_bursts']+=bool(res);stat['candidates']+=len(res)
                processing_ms.append((ended-started)*1000);latency_ms.append((ended-enqueued)*1000)
                if res and len(examples)<8:examples.append({'capture_id':cid,'capture_trigger_us':ts,'detections':res[:4]})
            if res and not args.quiet:
                print('DETECT',json.dumps({'capture_id':cid,'capture_trigger_us':ts,'sample_index':res[0][0],'short_score':round(res[0][2],3),'long_score':round(res[0][3],3),'candidates_in_burst':len(res)}),flush=True)
            work.task_done()
    examples=[];thread=threading.Thread(target=worker);thread.start();start=time.monotonic()
    try:
        print(request(22,'iq_stop'),flush=True)
        print(request(22,'iq_set freq=2412 gain=agc bw=0'),flush=True)
        print(request(22,f'iq_capture n=16380 rate=40000000 bits=8 count=0 interval_ms=0 dst={GROUP}:{PORT}'),flush=True)
        start=time.monotonic();end=start+args.seconds;next_print=start+5
        while time.monotonic()<end:
            try:data,peer=s.recvfrom(2048)
            except socket.timeout:continue
            if peer[0]!=h:continue
            if data[:4]==b'WJEV':stat['unexpected_event_packets']+=1;continue
            if data[:4]!=b'WJIQ' or len(data)<H.size:stat['other']+=1;continue
            f=H.unpack_from(data);_,ver,flags,bits,rate,cid,n,off,plen,hlen,crc,freq,ts,gain,bw,job,step,steps=f;payload=data[hlen:]
            if ver!=2 or bits!=8 or rate!=1 or n!=16380 or hlen!=44 or len(payload)!=plen or off+plen>n*2 or zlib.crc32(payload)&0xffffffff!=crc:
                stat['invalid']+=1;continue
            stat['sample_datagrams']+=1
            if cid not in open_caps:open_caps[cid]=[ts,bytearray(n*2),set(),0]
            c=open_caps[cid]
            if off not in c[2]:c[1][off:off+plen]=payload;c[2].add(off);c[3]+=plen
            else:stat['duplicate_fragments']+=1
            if c[3]==n*2:
                stat['complete']+=1
                try:work.put_nowait((cid,c[0],bytes(c[1]),time.monotonic()))
                except queue.Full:stat['worker_queue_drop']+=1
                del open_caps[cid]
            while len(open_caps)>100:
                del open_caps[next(iter(open_caps))];stat['incomplete_evicted']+=1
            now=time.monotonic()
            if now>=next_print:
                with lock: snap=dict(stat)
                print('progress',round(now-start,1),json.dumps(snap),'queued',work.qsize(),flush=True);next_print=now+5
    finally:
        elapsed=time.monotonic()-start
        try:
            print('stop',request(22,'iq_stop'),flush=True);time.sleep(.2)
            print('device',request(22,'iq_info').replace('\n',' | '),flush=True)
        except Exception as ex:print('control_error',ex,flush=True)
        work.put(None);thread.join();s.close();ctl.close()
    def percentile(x,q):return round(float(np.percentile(x,q)),3) if x else None
    print('RESULT',json.dumps({'duration_s':round(elapsed,3),'counts':dict(stat),'incomplete_remaining':len(open_caps),'processed_bursts_per_second':round(stat['processed']/elapsed,2),'processing_ms_p50':percentile(processing_ms,50),'processing_ms_p95':percentile(processing_ms,95),'enqueue_to_done_ms_p95':percentile(latency_ms,95),'examples':examples,'note':'preamble candidates, not decoded or identified frames; capture gaps remain'},default=float),flush=True)

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--offline');p.add_argument('--host',default='192.168.253.14');p.add_argument('--interface-ip',default='192.168.253.106');p.add_argument('--seconds',type=float,default=30);p.add_argument('--quiet',action='store_true',help='omit individual detection lines');a=p.parse_args()
    if a.offline:raise SystemExit(0 if run_offline(a.offline) else 1)
    live(a)
