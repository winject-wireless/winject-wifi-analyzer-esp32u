const $ = (s) => document.querySelector(s), $$ = (s) => [...document.querySelectorAll(s)];
const state = {frames: [], selected: null, csi: [], raw: [], power: [], spectrum: [], source: 'csi', powerSource: 'iq'};

class Device {
  constructor(onEvent) { this.onEvent = onEvent; this.mock = location.protocol === 'file:' || new URLSearchParams(location.search).has('mock'); }
  async connect() {
    if (this.mock) return this.startMock();
    const info = await fetch('/api/v1/info').then((r) => { if (!r.ok) throw Error(r.status); return r.json(); });
    this.onEvent({event: 'info', data: info});
    const ws = new WebSocket(`${location.protocol === 'https:' ? 'wss' : 'ws'}://${location.host}/api/v1/events`);
    ws.binaryType = 'arraybuffer';
    ws.onmessage = (e) => typeof e.data === 'string' && this.onEvent(JSON.parse(e.data));
    ws.onclose = () => setStatus(false, 'Disconnected');
    await new Promise((resolve, reject) => { ws.onopen = resolve; ws.onerror = reject; });
    setStatus(true, 'Device online');
  }
  async patch(config) { if (this.mock) return; await fetch('/api/v1/config', {method:'PATCH',headers:{'content-type':'application/json'},body:JSON.stringify(config)}); }
  async capture() { if (this.mock) { this.mockRaw(); return; } await fetch('/api/v1/captures',{method:'POST',headers:{'content-type':'application/json'},body:JSON.stringify({samples:16380,sample_rate_hz:16000000,bits:8})}); }
  async sweep(config) { if (this.mock) return this.mockSweep(config); await fetch('/api/v1/sweeps',{method:'POST',headers:{'content-type':'application/json'},body:JSON.stringify(config)}); }
  startMock() {
    setStatus(true, 'Demo data');
    this.onEvent({event:'info',data:{mode:'frames',frequency_mhz:2442}});
    let seq = 0;
    this.timer = setInterval(() => {
      seq++;
      const types = ['Beacon','Data','QoS Data','Probe Req','ACK'];
      const type = types[Math.floor(Math.random()*types.length)], rssi = -28-Math.floor(Math.random()*52);
      const bytes = Array.from({length: 44+Math.floor(Math.random()*180)}, () => Math.floor(Math.random()*256));
      const frame = {sequence:seq,time_us:performance.now()*1000,type,source:mac(seq*31),destination:type==='Beacon'?'ff:ff:ff:ff:ff:ff':mac(seq*17),rssi,noise:-94,channel:7,length:bytes.length,fcs:'ok',mpdu:btoa(String.fromCharCode(...bytes))};
      this.onEvent({event:'frame',data:frame});
      const points = Array.from({length:56},(_,i)=>{const a=(i/56)*Math.PI*2+Math.random()*.08,r=.45+Math.random()*.35;return [Math.cos(a)*r,Math.sin(a)*r]});
      this.onEvent({event:'frame.csi',data:{frame_sequence:seq,rssi,noise:-94,channel:7,points}});
      this.onEvent({event:'power',data:{source:'rssi',value:rssi,unit:'dBm',rms:rssi,peak:rssi+2,clipped:0,samples:1}});
    }, 430);
  }
  mockRaw() {
    const clusters = [[.65,.65],[-.65,.65],[.65,-.65],[-.65,-.65]];
    const points=Array.from({length:800},()=>{const c=clusters[Math.floor(Math.random()*4)];return[c[0]+gauss()*.09,c[1]+gauss()*.09]});
    this.onEvent({event:'raw.iq',data:{points,rssi:null,noise:-68,channel:7}});
    this.onEvent({event:'power',data:{source:'iq',value:-18.7,unit:'dBFS',rms:-18.7,peak:-9.3,clipped:.0012,samples:16380}});
    toast('Raw IQ snapshot complete');
  }
  async mockSweep({start_mhz,stop_mhz,step_mhz}) {
    const total=Math.floor((stop_mhz-start_mhz)/step_mhz)+1; state.spectrum=[];
    for(let i=0;i<total;i++) { await new Promise(r=>setTimeout(r,18)); const f=start_mhz+i*step_mhz, peaks=[1960,2412,2442,2462,2680]; let p=-91+Math.random()*7; for(const peak of peaks)p=Math.max(p,-20-Math.abs(f-peak)*1.8+Math.random()*4); this.onEvent({event:'sweep.tile',data:{frequency_mhz:f,power_dbfs:p,index:i,total}}); }
    toast('Sweep complete');
  }
}
function mac(n){return[0x24,0x6f,0x28,(n>>16)&255,(n>>8)&255,n&255].map(x=>x.toString(16).padStart(2,'0')).join(':')}
function gauss(){return(Math.random()+Math.random()+Math.random()+Math.random()-2)/2}
function setStatus(ok,text){$('#status-dot').classList.toggle('ok',ok);$('#status-text').textContent=text}
function toast(text){const el=$('#toast');el.textContent=text;el.classList.add('show');setTimeout(()=>el.classList.remove('show'),1800)}

function event(e) {
  if(e.event==='frame') { state.frames.unshift(e.data); state.frames=state.frames.slice(0,100); state.selected??=e.data; renderFrames(); $('#frame-rate').textContent='~2.3/s'; }
  if(e.event==='frame.csi') { state.csi=e.data.points; state.csiMeta=e.data; if(state.source==='csi') renderConstellation(); }
  if(e.event==='raw.iq') { state.raw=e.data.points; state.rawMeta=e.data; if(state.source==='raw') renderConstellation(); }
  if(e.event==='power') { state.power.push({t:performance.now(),...e.data}); state.power=state.power.slice(-180); renderPower(e.data); }
  if(e.event==='sweep.tile') { state.spectrum[e.data.index]=e.data; $('#sweep-progress').textContent=`${e.data.index+1} / ${e.data.total}`; renderSpectrum(); }
  if(e.event==='dropped') $('#dropped').textContent=e.data.frames??0;
}
const device=new Device(event);

$$('.nav').forEach(b=>b.onclick=()=>{ $$('.nav,.view').forEach(x=>x.classList.remove('active')); b.classList.add('active'); $(`#view-${b.dataset.view}`).classList.add('active'); resize(); });
$$('[data-source]').forEach(b=>b.onclick=()=>{$$('[data-source]').forEach(x=>x.classList.remove('active'));b.classList.add('active');state.source=b.dataset.source;renderConstellation()});
$$('[data-power]').forEach(b=>b.onclick=async()=>{$$('[data-power]').forEach(x=>x.classList.remove('active'));b.classList.add('active');state.powerSource=b.dataset.power;if(state.powerSource==='iq'&&!state.power.some(p=>p.source==='iq'))await device.capture();const latest=[...state.power].reverse().find(p=>p.source===state.powerSource);if(latest)renderPower(latest)});
$('#apply').onclick=async()=>{await device.patch({mode:$('#mode').value,frequency_mhz:+$('#frequency').value,gain:$('#gain').value==='hardware'?{mode:'hardware'}:{mode:'manual',index:+$('#gain').value}});toast('Receiver configuration applied')};
$('#capture-iq').onclick=async()=>{state.source='raw';$$('[data-source]').forEach(x=>x.classList.toggle('active',x.dataset.source==='raw'));await device.capture()};
$('#start-sweep').onclick=async()=>{const c={start_mhz:+$('#sweep-start').value,stop_mhz:+$('#sweep-stop').value,step_mhz:+$('#sweep-step').value,fft_bins:512,sample_rate_hz:16000000,detector:$('#detector').value.toLowerCase(),dwell_captures:4};if(c.start_mhz<1850||c.stop_mhz>2850)toast('Warning: sweep extends beyond measured ESP32 range');$('#scale-start').textContent=`${c.start_mhz} MHz`;$('#scale-stop').textContent=`${c.stop_mhz} MHz`;await device.sweep(c)};

function renderFrames(){const body=$('#frames-body');body.innerHTML=state.frames.map((f,i)=>`<tr data-i="${i}" class="${state.selected?.sequence===f.sequence?'selected':''}"><td>${(f.time_us/1e6).toFixed(3)}</td><td class="type">${f.type}</td><td>${f.source}</td><td>${f.destination}</td><td class="rssi">${f.rssi} dBm</td><td>${f.length}</td></tr>`).join('');body.querySelectorAll('tr').forEach(r=>r.onclick=()=>{state.selected=state.frames[+r.dataset.i];renderFrames();renderDetail()});renderDetail()}
function renderDetail(){const f=state.selected;if(!f)return;$('#frame-seq').textContent=`Sequence #${f.sequence}`;$('#frame-fields').innerHTML=[['Type',f.type],['RSSI',`${f.rssi} dBm`],['Source',f.source],['Destination',f.destination],['Channel',f.channel],['FCS',f.fcs.toUpperCase()]].map(([k,v])=>`<div><dt>${k}</dt><dd>${v}</dd></div>`).join('');const bytes=Uint8Array.from(atob(f.mpdu),c=>c.charCodeAt(0));$('#frame-hex').textContent=[...bytes].map((v,i)=>(i%16?'':'\n'+i.toString(16).padStart(4,'0')+'  ')+v.toString(16).padStart(2,'0')+' ').join('').trim()}

function canvas(id){const c=$(id),dpr=devicePixelRatio||1,r=c.getBoundingClientRect();if(c.width!==Math.round(r.width*dpr)||c.height!==Math.round(r.height*dpr)){c.width=Math.round(r.width*dpr);c.height=Math.round(r.height*dpr)}const x=c.getContext('2d');x.setTransform(dpr,0,0,dpr,0,0);return{x,w:r.width,h:r.height}}
function grid(ctx,w,h,vertical=8,horizontal=6){ctx.strokeStyle='#1d303b';ctx.lineWidth=1;for(let i=1;i<vertical;i++){ctx.beginPath();ctx.moveTo(i*w/vertical,0);ctx.lineTo(i*w/vertical,h);ctx.stroke()}for(let i=1;i<horizontal;i++){ctx.beginPath();ctx.moveTo(0,i*h/horizontal);ctx.lineTo(w,i*h/horizontal);ctx.stroke()}}
function renderConstellation(){const{x,w,h}=canvas('#constellation-canvas');x.clearRect(0,0,w,h);grid(x,w,h,8,8);x.strokeStyle='#38505d';x.beginPath();x.moveTo(w/2,0);x.lineTo(w/2,h);x.moveTo(0,h/2);x.lineTo(w,h/2);x.stroke();const raw=state.source==='raw',points=raw?state.raw:state.csi,meta=raw?state.rawMeta:state.csiMeta;x.fillStyle=raw?'#46a4ff99':'#36e5d2aa';for(const [i,q] of points){x.beginPath();x.arc(w/2+i*w*.38,h/2-q*h*.38,raw?2:2.5,0,Math.PI*2);x.fill()}$('#constellation-kind').textContent=raw?'RAW BASEBAND IQ':'FRAME-ALIGNED CSI';$('#constellation-title').textContent=raw?'ESP-SDR snapshot':`Channel state for frame #${meta?.frame_sequence??'—'}`;$('#constellation-copy').textContent=raw?'True baseband samples from a bounded ESP-SDR capture. This snapshot is not correlated with a MAC-decoded frame.':'CSI points are complex channel estimates per OFDM subcarrier. They are associated with a received frame, but are not decoded payload symbols.';$('#c-rssi').textContent=meta?.rssi?`${meta.rssi} dBm`:'n/a';$('#c-points').textContent=points.length||'—';$('#c-noise').textContent=meta?.noise?`${meta.noise} dBm`:'—';$('#c-channel').textContent=meta?.channel??'—'}
function renderPower(p){if(p.source!==state.powerSource)return;$('#power-value').textContent=p.value.toFixed(1);$('#power-unit').textContent=p.unit;$('#p-rms').textContent=p.rms.toFixed(1)+' '+p.unit;$('#p-peak').textContent=p.peak.toFixed(1)+' '+p.unit;$('#p-clip').textContent=(p.clipped*100).toFixed(2)+'%';$('#p-samples').textContent=p.samples;drawGauge(p.value,p.unit);drawHistory()}
function drawGauge(v,unit){const{x,w,h}=canvas('#gauge-canvas');x.clearRect(0,0,w,h);const min=unit==='dBm'?-100:-80,max=0,t=Math.max(0,Math.min(1,(v-min)/(max-min))),cx=w/2,cy=h*.86,r=Math.min(w*.38,h*.7);x.lineWidth=16;x.lineCap='round';x.strokeStyle='#1c2d37';x.beginPath();x.arc(cx,cy,r,Math.PI,2*Math.PI);x.stroke();const g=x.createLinearGradient(cx-r,0,cx+r,0);g.addColorStop(0,'#46a4ff');g.addColorStop(.7,'#36e5d2');g.addColorStop(1,'#ff6477');x.strokeStyle=g;x.beginPath();x.arc(cx,cy,r,Math.PI,Math.PI+Math.PI*t);x.stroke()}
function drawHistory(){const{x,w,h}=canvas('#power-canvas');x.clearRect(0,0,w,h);grid(x,w,h);const vals=state.power.filter(p=>p.source===state.powerSource).slice(-100);if(vals.length<2)return;const min=Math.min(...vals.map(p=>p.value))-5,max=Math.max(...vals.map(p=>p.value))+5;x.strokeStyle='#36e5d2';x.lineWidth=2;x.beginPath();vals.forEach((p,i)=>{const px=i*w/Math.max(1,vals.length-1),py=h-(p.value-min)/(max-min)*h;i?x.lineTo(px,py):x.moveTo(px,py)});x.stroke()}
function renderSpectrum(){const{x,w,h}=canvas('#spectrum-canvas');x.clearRect(0,0,w,h);grid(x,w,h,10,8);if(!state.spectrum.length)return;const vals=state.spectrum.filter(Boolean),min=-100,max=-10;x.fillStyle='#36e5d222';x.strokeStyle='#36e5d2';x.lineWidth=1.5;x.beginPath();vals.forEach((p,i)=>{const px=i*w/Math.max(1,vals.length-1),py=h-(p.power_dbfs-min)/(max-min)*h;i?x.lineTo(px,py):x.moveTo(px,py)});x.lineTo(w,h);x.lineTo(0,h);x.closePath();x.fill();x.stroke()}
function resize(){renderConstellation();drawHistory();renderSpectrum()}addEventListener('resize',resize);
device.connect().catch(()=>{setStatus(false,'API unavailable · demo');device.startMock()});
