#!/usr/bin/env python3
"""Raw-stack Ethernet benchmark; rates and goodput count UDP payload bytes."""
import argparse, socket, struct, threading, time

p=argparse.ArgumentParser()
p.add_argument('host'); p.add_argument('--direction', choices=['tx','rx','both'], default='both', help='relative to ESP32')
p.add_argument('--rate',type=int,default=30000,help='target kbit/s per active direction')
p.add_argument('--seconds',type=float,default=5)
a=p.parse_args()
if not (1<=a.rate<=100000 and .1<=a.seconds<=60): p.error('rate 1..100000 kbit/s, duration 0.1..60 seconds')
ctl=socket.socket(socket.AF_INET,socket.SOCK_DGRAM); ctl.settimeout(2)
def command(s):
    ctl.sendto(s.encode(),(a.host,5500))
    while True:
        data,peer=ctl.recvfrom(2048)
        if peer[0]==a.host and peer[1]==5500: return data.decode()
def stats():
    s=command('STAT')
    if not s.startswith('STAT '): raise RuntimeError(s)
    return {k:int(v) for k,v in (item.split('=') for item in s.split()[1:])}
s=socket.socket(socket.AF_INET,socket.SOCK_DGRAM); s.setsockopt(socket.SOL_SOCKET,socket.SO_RCVBUF,4*1024*1024); s.bind(('0.0.0.0',5501)); s.settimeout(.1)
print(command('RESET'))
start_stats=stats(); sent=0; send_errors=0; ids=set(); rxbytes=0; done=threading.Event()
def receive():
    global rxbytes
    while not done.is_set():
        try: data,peer=s.recvfrom(2048)
        except socket.timeout: continue
        if peer[0]==a.host and len(data)>=8 and data[:4]==b'ETST':
            seq=struct.unpack_from('!I',data,4)[0]
            if seq not in ids: ids.add(seq); rxbytes+=len(data)
thread=threading.Thread(target=receive); thread.start()
try:
    if a.direction in ('tx','both'):
        response=command(f'TX {a.rate} {round(a.seconds*1000)}')
        if not response.startswith('OK '): raise RuntimeError(response)
    begin=time.monotonic(); end=begin+a.seconds
    if a.direction in ('rx','both'):
        payload=bytearray(1400); payload[:4]=b'ETST'; seq=0
        while True:
            now=time.monotonic()
            if now>=end: break
            due=begin+seq*len(payload)*8/(a.rate*1000)
            if due>now: time.sleep(min(due-now,.01)); continue
            struct.pack_into('!I',payload,4,seq); seq+=1
            try: s.sendto(payload,(a.host,5501)); sent+=1
            except OSError: send_errors+=1
    else: time.sleep(a.seconds)
    time.sleep(.7)
    end_stats=stats()
    d={k:end_stats[k]-start_stats[k] for k in end_stats if k!='running'}
    print(f'target={a.rate/1000:.3f} Mbit/s per direction duration={a.seconds:.3f} s payload=1400 bytes')
    if a.direction in ('rx','both'):
        print(f'host -> ESP32: offered={sent*1400*8/a.seconds/1e6:.3f} Mbit/s goodput={d["rx_bytes"]*8/a.seconds/1e6:.3f} Mbit/s sent={sent} received={d["rx_packets"]} loss={max(0,sent-d["rx_packets"])} ({max(0,sent-d["rx_packets"])/sent*100 if sent else 0:.2f}%) host_send_errors={send_errors} net_rx_queue_drops={d["net_rx_drop"]}')
    if a.direction in ('tx','both'):
        ok=d['tx_ok']; loss=max(0,ok-len(ids))
        print(f'ESP32 -> host: offered={d["tx_attempted"]*1400*8/a.seconds/1e6:.3f} Mbit/s goodput={rxbytes*8/a.seconds/1e6:.3f} Mbit/s attempted={d["tx_attempted"]} accepted={ok} received_unique={len(ids)} loss_after_accept={loss} ({loss/ok*100 if ok else 0:.2f}%) device_send_errors={d["tx_fail"]}')
    if end_stats['running']: print('WARNING: device sender still running; results incomplete')
finally:
    done.set(); thread.join(); s.close(); ctl.close()
