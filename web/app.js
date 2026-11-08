// Winject IQ Analyzer browser client. Talks to the host server (server.py)
// over one WebSocket at /ws; the ESP32U itself only streams samples.
const $ = (s) => document.querySelector(s), $$ = (s) => [...document.querySelectorAll(s)];
const state = {
  status: null, live: null, liveAt: 0, snapshot: null, points: 'corrected',
  history: [], sweep: {tiles: [], range: null}, peakHold: null,
  frames: [], verifiedFrames: [], selectedFrame: null, liveCandidate: null, liveCandidateAt: 0, filters: {addr1: '', addr2: '', addr3: ''}, fcsOnly: false, framePlot: 'channel', calibration: null,
};
let ws = null;
const preferencesKey = 'winject-analyzer-view-v1';
function savePreferences() {
  const activeView = $('.nav.active');
  const activeDetector = $('[data-detector].active');
  const preferences = {
    view: activeView?.dataset.view, detector: activeDetector?.dataset.detector,
    framePlot: state.framePlot, fcsOnly: state.fcsOnly, filters: state.filters,
    filterInputs: Object.fromEntries(['addr1', 'addr2', 'addr3'].map(k => [k, $(`#filter-${k}`).value])),
  };
  try { localStorage.setItem(preferencesKey, JSON.stringify(preferences)); } catch (_) { /* Storage may be unavailable. */ }
}
function restorePreferences() {
  let saved;
  try { saved = JSON.parse(localStorage.getItem(preferencesKey)); } catch (_) { return; }
  if (!saved || typeof saved !== 'object') return;
  const view = $$('.nav').find(b => b.dataset.view === saved.view);
  if (view) {
    $$('.nav,.view').forEach(x => x.classList.remove('active'));
    view.classList.add('active');
    $(`#view-${view.dataset.view}`).classList.add('active');
    const settings = $('#capture-settings');
    if (view.dataset.view === 'live') $('#spectrum-settings').appendChild(settings);
    else if (view.dataset.view === 'calibration') $('#calibration-settings').appendChild(settings);
    else $('.capture-column').prepend(settings);
  }
  const detector = $$('[data-detector]').find(b => b.dataset.detector === saved.detector);
  if (detector) $$('[data-detector]').forEach(b => b.classList.toggle('active', b === detector));
  const plot = $$('[data-frame-plot]').find(b => b.dataset.framePlot === saved.framePlot);
  if (plot) { state.framePlot = saved.framePlot; $$('[data-frame-plot]').forEach(b => b.classList.toggle('active', b === plot)); }
  state.fcsOnly = saved.fcsOnly === true;
  $('#filter-fcs').checked = state.fcsOnly;
  for (const key of ['addr1', 'addr2', 'addr3']) {
    const value = typeof saved.filters?.[key] === 'string' ? normalizeMac(saved.filters[key]) : '';
    state.filters[key] = value || '';
    $(`#filter-${key}`).value = typeof saved.filterInputs?.[key] === 'string' ? saved.filterInputs[key] : state.filters[key];
  }
  if (['addr1', 'addr2', 'addr3'].some(k => normalizeMac($(`#filter-${k}`).value) === null))
    $('#filter-error').textContent = 'Use six hexadecimal octets, for example ca:fe:ba:be:00:01.';
}


function connect() {
  ws = new WebSocket(`${location.protocol === 'https:' ? 'wss' : 'ws'}://${location.host}/ws`);
  ws.onopen = () => { setStatus(true, 'Host connected'); renderFrames(); };
  ws.onclose = () => { setStatus(false, 'Reconnecting…'); renderConstellation(); renderChannelResponse(); setTimeout(connect, 1500); };
  ws.onmessage = (e) => handle(JSON.parse(e.data));
}
function send(msg) { if (ws?.readyState === 1) ws.send(JSON.stringify(msg)); }
function setStatus(ok, text) { $('#status-dot').classList.toggle('ok', ok); $('#status-text').textContent = text; }
function toast(text) { const el = $('#toast'); el.textContent = text; el.classList.add('show'); clearTimeout(toast.t); toast.t = setTimeout(() => el.classList.remove('show'), 2200); }
const mhz = (hz) => `${(hz / 1e6).toFixed(3)} MHz`;

function handle(m) {
  const d = m.data;
  if (m.event === 'status') applyStatus(d);
  else if (m.event === 'live') onLive(d);
  else if (m.event === 'frames') onFrames(d);
  else if (m.event === 'frames.clear') { state.frames = []; state.verifiedFrames = []; state.selectedFrame = null; state.liveCandidate = null; state.liveCandidateAt = 0; if (state.points === 'frame') selectPoints('corrected'); renderFrames(); renderConstellation(); renderChannelResponse(); }
  else if (m.event === 'snapshot') { state.snapshot = d; selectPoints('snapshot'); const a = $('#download'); a.href = d.download; a.hidden = false; toast(`Snapshot ${d.capture_id}: ${d.samples} samples`); }
  else if (m.event === 'sweep.start') { state.sweep = {tiles: [], range: [(d.start_mhz - d.step_mhz / 2) * 1e6, (d.stop_mhz + d.step_mhz / 2) * 1e6]}; $('#sweep-progress').textContent = `0 / ${d.steps}`; $('#scale-start').textContent = `${d.start_mhz} MHz`; $('#scale-stop').textContent = `${d.stop_mhz} MHz`; }
  else if (m.event === 'sweep.tile') { state.sweep.tiles[d.index] = d; $('#sweep-progress').textContent = `${d.index + 1} / ${d.total} · gain ${d.gain_index}`; renderSweep(); }
  else if (m.event === 'sweep.done') { $('#sweep-progress').textContent = `${d.state} · ${d.seconds} s`; toast(`Sweep ${d.state} in ${d.seconds} s`); }
  else if (m.event === 'calibration.progress') { $('#calibration-auto-cancel').hidden = false; $('#calibration-auto-progress').textContent = `Measuring gain ${d.gain} of ${d.total - 1} · ${d.accepted} accepted · ${d.skipped} skipped`; }
  else if (m.event === 'calibration.done') {
    $('#calibration-auto-cancel').hidden = true;
    const points = Array.isArray(d.points) ? d.points : [];
    if (points.length) {
      if (!state.calibration) state.calibration = {version:1, points:[]};
      for (const point of points) state.calibration.points = state.calibration.points.filter(p => !(p.hz === point.hz && sameCalibrationSettings(p.settings || state.calibration.settings, point.settings)));
      state.calibration.points.push(...points); state.calibration.points.sort((a,b) => a.hz-b.hz);
      storeCalibration(); renderCalibrationTable(); renderLive(); renderCalibration();
    }
    $('#calibration-auto-progress').textContent = d.error ? `Calibration failed: ${d.error}` : d.cancelled ? 'Calibration cancelled.' : `Finished: ${points.length} gains calibrated, ${d.skipped.length} skipped (clipped, weak, or gain mismatch).`;
  }
  else if (m.event === 'error') toast(d.message);
}

function applyStatus(s) {
  const first = !state.status;
  state.status = s;
  const r = s.radio || {};
  setStatus(!r.error, r.error ? `Radio: ${r.error}` : `ESP32U ${s.radio_ip} · ${s.mode}`);
  $('#rate-metric').textContent = `${s.captures_per_s}/s`;
  $('#lost').textContent = `${s.lost}${s.errors ? ` · ${s.errors} err` : ''}`;
  $('#run').textContent = s.mode === 'live' ? 'Pause' : 'Play';
  $('#start-sweep').textContent = s.mode === 'sweep' ? 'Stop sweep' : 'Start sweep';
  if (state.points !== 'frame' && state.points !== 'snapshot') { renderConstellation(); renderChannelResponse(); }
  const detector = $('#detector-status');
  const sampleRates = Array.isArray(s.detector_sample_rates_hz) ? s.detector_sample_rates_hz.filter(Number.isFinite).map(r => `${r / 1e6} MS/s`).join(', ') : '';
  const formats = Array.isArray(s.decoder_formats) ? s.decoder_formats.filter(v => typeof v === 'string').join(', ') : '';
  const rates = Array.isArray(s.decoder_rates_mbps) ? s.decoder_rates_mbps.filter(Number.isFinite).join(', ') : '';
  const backend = typeof s.decoder_backend === 'string' && s.decoder_backend ? s.decoder_backend : '';
  const capability = [backend, formats, rates ? `${rates} Mb/s` : ''].filter(Boolean).join(' · ');
  detector.textContent = s.detector_error ? `Detector error: ${s.detector_error}` : s.decoder_error ? `Decoder error: ${s.decoder_error}` : !s.detector_supported ? `Frame detection unavailable at this sample rate${sampleRates ? `; supported: ${sampleRates}` : ''}. Raw IQ remains available.` : `Detector: ${s.detector_processed ?? 0} bursts, ${s.detector_dropped ?? 0} skipped · Decoder${capability ? ` (${capability})` : ''}: ${s.decoder_decoded ?? 0} FCS verified / ${s.decoder_attempted ?? 0} attempted, ${s.decoder_dropped ?? 0} skipped`;
  if (formats.includes('DSSS')) detector.textContent += s.dsss_error ? ` · DSSS error: ${s.dsss_error}` : ` · DSSS/CCK: ${s.dsss_decoded ?? 0} FCS verified / ${s.dsss_attempted ?? 0} bursts, ${s.dsss_dropped ?? 0} skipped`;


  if (r.gain_max) $('#gain-index').max = r.gain_max;
  if (first) {
    const c = s.config;
    $('#freq').value = c.freq_mhz; $('#rate').value = c.rate_hz; $('#bw').value = c.bw_mhz;
    $('#n').value = c.n; $('#bits').value = c.bits;
    $('#gain').value = c.gain === 'agc' ? 'agc' : 'manual';
    if (c.gain !== 'agc') $('#gain-index').value = c.gain;
  }
}

function onLive(d) {
  state.live = d;
  state.liveAt = performance.now();
  state.history.push({t: performance.now(), mean: d.power.mean_dbfs, peak: d.power.peak_dbfs});
  state.history = state.history.slice(-300);
  $('#live-meta').textContent = `${mhz(d.frequency_hz)} · ${d.sample_rate_hz / 1e6} MS/s · ${d.samples} × IQ${d.bits} · gain ${d.gain_index}${d.agc ? ' (AGC)' : ''}`;
  renderLive(); renderCalibration(); renderPower(d); if (state.points === 'raw' || state.points === 'corrected') { renderConstellation(); renderChannelResponse(); }
}

function config() {
  return {
    op: 'config', freq_mhz: +$('#freq').value, rate_hz: +$('#rate').value,
    gain: $('#gain').value === 'agc' ? 'agc' : +$('#gain-index').value,
    bw_mhz: +$('#bw').value, n: +$('#n').value, bits: +$('#bits').value,
  };
}

// --- canvases -------------------------------------------------------------------
function canvas(id) {
  const c = $(id), dpr = devicePixelRatio || 1, r = c.getBoundingClientRect();
  if (c.width !== Math.round(r.width * dpr) || c.height !== Math.round(r.height * dpr)) { c.width = Math.round(r.width * dpr); c.height = Math.round(r.height * dpr); }
  const x = c.getContext('2d'); x.setTransform(dpr, 0, 0, dpr, 0, 0); return {x, w: r.width, h: r.height};
}
function grid(ctx, w, h, v = 10, hz = 6) {
  ctx.strokeStyle = '#1d303b'; ctx.lineWidth = 1;
  for (let i = 1; i < v; i++) { ctx.beginPath(); ctx.moveTo(i * w / v, 0); ctx.lineTo(i * w / v, h); ctx.stroke(); }
  for (let i = 1; i < hz; i++) { ctx.beginPath(); ctx.moveTo(0, i * h / hz); ctx.lineTo(w, i * h / hz); ctx.stroke(); }
}
function yLabels(ctx, h, min, max, unit = 'dBFS') {
  ctx.fillStyle = '#7f97a3'; ctx.font = '11px monospace';
  for (let k = 0; k <= 4; k++) { const v = max - (max - min) * k / 4; ctx.fillText(`${v.toFixed(0)} ${unit}`, 6, Math.max(12, k * h / 4 - 4)); }
}
function trace(ctx, xs, ys, X, Y, h, color, fill) {
  ctx.strokeStyle = color; ctx.lineWidth = 1.3; ctx.beginPath();
  xs.forEach((f, i) => i ? ctx.lineTo(X(f), Y(ys[i])) : ctx.moveTo(X(f), Y(ys[i])));
  if (fill) { ctx.stroke(); ctx.lineTo(X(xs[xs.length - 1]), h); ctx.lineTo(X(xs[0]), h); ctx.closePath(); ctx.fillStyle = fill; ctx.fill(); } else ctx.stroke();
}

function renderLive() {
  const d = state.live; if (!d) return;
  const {x, w, h} = canvas('#live-spectrum'); x.clearRect(0, 0, w, h); grid(x, w, h);
  const s = d.spectrum, n = s.db.length, freqs = s.db.map((_, i) => s.start_hz + i * s.bin_hz);
  let db = s.db;
  if ($('[data-detector].active').dataset.detector === 'peak') {
    if (!state.peakHold || state.peakHold.length !== n) state.peakHold = db.slice();
    state.peakHold = state.peakHold.map((v, i) => Math.max(v, db[i])); db = state.peakHold;
  } else state.peakHold = null;
  const calibrated = calibrationMatches(d);
  const offsets = calibrated ? calibrationOffsets() : [];
  if (calibrated) db = db.map((v, i) => v + calibrationOffset(freqs[i], offsets));
  const min = Math.floor(Math.min(...db) / 10) * 10 - 5, max = Math.max(Math.ceil(Math.max(...db) / 10) * 10 + 5, min + 10);
  const X = (f) => (f - freqs[0]) / (freqs[n - 1] - freqs[0]) * w, Y = (v) => h - (v - min) / (max - min) * h;
  trace(x, freqs, db, X, Y, h, '#36e5d2', '#36e5d222');
  const t = d.power; x.fillStyle = '#ff6477'; x.beginPath(); x.arc(X(t.tone_hz), Y(t.tone_dbfs + (calibrated ? calibrationOffset(t.tone_hz, offsets) : 0)), 3.5, 0, 7); x.fill();
  yLabels(x, h, min, max, calibrated ? 'dBm' : 'dBFS');
  $('#live-lo').textContent = mhz(freqs[0]); $('#live-hi').textContent = mhz(freqs[n - 1]);
  const e = canvas('#live-envelope'); e.x.clearRect(0, 0, e.w, e.h); grid(e.x, e.w, e.h, 10, 3);
  const env = d.envelope, emin = -60, emax = 5, EY = (v) => e.h - (Math.max(emin, v) - emin) / (emax - emin) * e.h;
  trace(e.x, env.map((_, i) => i), env, (i) => i / (env.length - 1) * e.w, EY, e.h, '#46a4ff', '#46a4ff22');
  yLabels(e.x, e.h, emin, emax);
  $('#env-span').textContent = `${(d.samples / d.sample_rate_hz * 1e6).toFixed(0)} µs`;
}

// Calibration offsets convert a spectrum bin from dBFS to known input dBm.
const calibrationKey = 'winject-spectrum-calibration-v1';
function calibrationSettings(d) {
  const c = state.status?.config;
  return c && d && Number.isFinite(d.gain_index) ? {gain: Number(d.gain_index), rate_hz: d.sample_rate_hz, nfft: d.spectrum.db.length, samples: d.samples, detector: c.detector, bw_mhz: Number(c.bw_mhz), bits: d.bits} : null;
}
function sameCalibrationSettings(a,b) { return !!(a && b && ['gain','rate_hz','nfft','samples','detector','bw_mhz','bits'].every(k => a[k] === b[k])); }
function matchingCalibrationPoints(d) { const settings=calibrationSettings(d); return (state.calibration?.points || []).filter(p => sameCalibrationSettings(settings, p.settings || state.calibration.settings)); }
function calibrationMatches(d) { return matchingCalibrationPoints(d).length > 0; }
function validCalibration(c) {
  const validSettings = s => s && ['gain','rate_hz','nfft','samples','bw_mhz','bits'].every(k => Number.isFinite(s[k])) && ['average','peak'].includes(s.detector);
  return c?.version === 1 && Array.isArray(c.points) && c.points.length > 0 && c.points.length <= 1000 && c.points.every(p => validSettings(p.settings || c.settings) && Number.isFinite(p.hz) && p.hz > 0 && Number.isFinite(p.measured_dbfs) && Number.isFinite(p.actual_dbm));
}
function storeCalibration() { try { if (state.calibration) localStorage.setItem(calibrationKey, JSON.stringify(state.calibration)); else localStorage.removeItem(calibrationKey); } catch (_) {} }
function loadCalibration() { try { const c = JSON.parse(localStorage.getItem(calibrationKey)); if (validCalibration(c)) { c.points.sort((a,b) => a.hz-b.hz); state.calibration = c; } } catch (_) {} renderCalibrationTable(); }
function calibrationOffsets() {
  const groups = new Map();
  for (const p of matchingCalibrationPoints(state.live)) { const group=groups.get(p.hz) || []; group.push(p.actual_dbm-p.measured_dbfs); groups.set(p.hz,group); }
  return [...groups].map(([hz,values]) => ({hz, offset:values.reduce((a,b)=>a+b,0)/values.length})).sort((a,b)=>a.hz-b.hz);
}
function calibrationOffset(hz, points=calibrationOffsets()) {
  if (hz <= points[0].hz) return points[0].offset;
  if (hz >= points[points.length-1].hz) return points[points.length-1].offset;
  const i = points.findIndex(p => p.hz >= hz), a = points[i-1], b = points[i];
  return a.offset + (b.offset-a.offset) * (hz-a.hz)/(b.hz-a.hz);
}
function renderCalibrationTable() {
  const body = $('#calibration-body'); body.replaceChildren();
  for (const point of state.calibration?.points || []) {
    const row = document.createElement('tr');
    for (const value of [(point.hz/1e6).toFixed(3), `${point.measured_dbfs.toFixed(1)} dBFS`, `${point.actual_dbm.toFixed(1)} dBm`, String((point.settings || state.calibration.settings).gain)]) { const cell = document.createElement('td'); cell.textContent = value; row.appendChild(cell); }
    const cell = document.createElement('td'), button = document.createElement('button'); button.className = 'secondary'; button.textContent = 'Remove'; button.onclick = () => { state.calibration.points = state.calibration.points.filter(p => p !== point); if (!state.calibration.points.length) state.calibration = null; storeCalibration(); renderCalibrationTable(); renderLive(); renderCalibration(); }; cell.appendChild(button); row.appendChild(cell); body.appendChild(row);
  }
  const d = state.live;
  $('#calibration-status').textContent = !state.calibration ? 'No calibration loaded. Spectrum is in dBFS.' : calibrationMatches(d) ? 'Active. Matching points interpolate by frequency; repeated frequencies are averaged. Outside points, the nearest offset is used.' : 'Inactive: select the gain, rate, samples, detector, bandwidth and format used for these points. AGC can change the gain.';
}
function renderCalibration() {
  const status = $('#calibration-status');
  status.textContent = !state.calibration ? 'No calibration loaded. Spectrum is in dBFS.' : calibrationMatches(state.live) ? 'Active. Matching points interpolate by frequency; repeated frequencies are averaged. Outside points, the nearest offset is used.' : 'Inactive: select the gain, rate, samples, detector, bandwidth and format used for these points. AGC can change the gain.';
  const d = state.live; if (!d?.spectrum?.db?.length) return;
  const {x,w,h} = canvas('#calibration-spectrum'); if (!w || !h) return;
  x.clearRect(0,0,w,h); grid(x,w,h);
  const s=d.spectrum, n=s.db.length, freq=i=>s.start_hz+i*s.bin_hz, raw=s.db;
  const active=calibrationMatches(d), offsets=active ? calibrationOffsets() : [];
  const values=active ? raw.map((v,i)=>v+calibrationOffset(freq(i),offsets)) : raw;
  const min=Math.floor(Math.min(...values)/10)*10-5, max=Math.max(min+10,Math.ceil(Math.max(...values)/10)*10+5), X=i=>i/(n-1)*w, Y=v=>h-(v-min)/(max-min)*h;
  trace(x, values.map((_,i)=>i), values, X, Y, h, '#36e5d2', '#36e5d222'); yLabels(x,h,min,max,active ? 'dBm' : 'dBFS');
  let peak=0; for (let i=1;i<n;i++) if (raw[i]>raw[peak]) peak=i;
  x.fillStyle='#ff6477'; x.beginPath(); x.arc(X(peak),Y(values[peak]),4,0,7); x.fill();
  x.font='12px monospace'; x.textAlign='right'; x.textBaseline='top'; x.fillStyle='#e8f0f4';
  x.fillText(`Peak ${mhz(freq(peak))} · ${raw[peak].toFixed(1)} dBFS`,w-10,10);
  x.fillText(active ? `Calibrated ${values[peak].toFixed(1)} dBm` : 'Calibrated dBm unavailable for current settings',w-10,27);
  x.textAlign='start'; x.textBaseline='alphabetic';
  for (const p of matchingCalibrationPoints(d)) { const i=(p.hz-s.start_hz)/s.bin_hz; if (i>=0 && i<n) { x.fillStyle='#ffbf5f'; x.beginPath(); x.arc(X(i),Y(p.measured_dbfs+(active ? calibrationOffset(p.hz,offsets) : 0)),5,0,7); x.fill(); } }
  $('#calibration-lo').textContent=mhz(freq(0)); $('#calibration-hi').textContent=mhz(freq(n-1));
}
let autoTone = null;
$('#calibration-auto').onclick = () => {
  const s = state.live?.spectrum;
  if (state.status?.mode !== 'live' || !s?.db?.length) { toast('Start live capture to calibrate'); return; }
  let peak = 0; for (let i = 1; i < s.db.length; i++) if (s.db[i] > s.db[peak]) peak = i;
  autoTone = s.start_hz + peak * s.bin_hz;
  $('#calibration-auto-tone').textContent = `Strongest tone: ${mhz(autoTone)}. Keep the tone steady throughout the sweep.`;
  $('#calibration-auto-power').value = ''; $('#calibration-auto-form').hidden = false; $('#calibration-auto-power').focus();
};
$('#calibration-auto-close').onclick = () => { $('#calibration-auto-form').hidden = true; };
$('#calibration-auto-form').onsubmit = e => {
  e.preventDefault(); const value = $('#calibration-auto-power').value;
  if (!value.trim() || !Number.isFinite(Number(value)) || !Number.isFinite(autoTone)) { toast('Enter a valid tone power'); return; }
  if (ws?.readyState !== WebSocket.OPEN) { toast('Host disconnected'); return; }
  send({op:'calibration.start', power_dbm:Number(value), tone_hz:autoTone});
  $('#calibration-auto-form').hidden = true; $('#calibration-auto-progress').textContent = 'Starting gain sweep…';
};
$('#calibration-auto-cancel').onclick = () => send({op:'calibration.cancel'});
let pendingCalibration = null;
function addCalibrationPoint(i) {
  const d=state.live, settings=calibrationSettings(d);
  if (!settings) { toast('Waiting for a live capture with a reported gain'); return; }
  const s=d.spectrum;
  if (!s?.db?.length || !Number.isInteger(i) || i < 0 || i >= s.db.length) return;
  pendingCalibration={hz:s.start_hz+i*s.bin_hz,measured_dbfs:s.db[i],settings};
  $('#calibration-selection').textContent = `${mhz(pendingCalibration.hz)} · ${pendingCalibration.measured_dbfs.toFixed(1)} dBFS · gain ${settings.gain}`;
  $('#calibration-power').value=''; $('#calibration-entry').hidden=false; $('#calibration-power').focus();
}
$('#calibration-entry').onsubmit = e => {
  e.preventDefault();
  const value=$('#calibration-power').value, actual=Number(value);
  if (!pendingCalibration || !value.trim() || !Number.isFinite(actual)) { toast('Enter a valid power in dBm'); return; }
  if (!state.calibration) state.calibration={version:1,points:[]};
  if (state.calibration.points.length >= 1000) { toast('Calibration is full'); return; }
  state.calibration.points.push({...pendingCalibration,actual_dbm:actual}); state.calibration.points.sort((a,b)=>a.hz-b.hz);
  pendingCalibration=null; $('#calibration-entry').hidden=true;
  storeCalibration(); renderCalibrationTable(); renderLive(); renderCalibration();
};
$('#calibration-cancel').onclick = () => { pendingCalibration=null; $('#calibration-entry').hidden=true; };
$('#calibration-spectrum').onclick = e => {
  const s=state.live?.spectrum, r=e.currentTarget.getBoundingClientRect();
  if (!s?.db?.length || !r.width) { toast('Start live capture to calibrate'); return; }
  addCalibrationPoint(Math.max(0,Math.min(s.db.length-1,Math.round((e.clientX-r.left)/r.width*(s.db.length-1)))));
};
$('#calibration-peak').onclick = () => {
  const values=state.live?.spectrum?.db;
  if (!values?.length) { toast('Start live capture to calibrate'); return; }
  let peak=0; for (let i=1;i<values.length;i++) if (values[i]>values[peak]) peak=i;
  addCalibrationPoint(peak);
};
$('#calibration-reset').onclick=()=>{ state.calibration=null; storeCalibration(); renderCalibrationTable(); renderLive(); renderCalibration(); };
$('#calibration-load').onclick=()=>$('#calibration-file').click();
$('#calibration-file').onchange=async e=>{ try { const c=JSON.parse(await e.target.files[0].text()); if (!validCalibration(c)) throw Error('Invalid calibration'); c.points.sort((a,b)=>a.hz-b.hz); state.calibration=c; storeCalibration(); renderCalibrationTable(); renderLive(); renderCalibration(); toast('Calibration loaded'); } catch (_) { toast('Invalid calibration file'); } finally { e.target.value=''; } };
$('#calibration-save').onclick=()=>{ if (!state.calibration) { toast('No calibration to save'); return; } const url=URL.createObjectURL(new Blob([JSON.stringify(state.calibration,null,2)],{type:'application/json'})), a=document.createElement('a'); a.href=url; a.download='winject-calibration.json'; a.click(); setTimeout(()=>URL.revokeObjectURL(url),1000); };

function frameId(f) { return `${f.capture_id}:${f.start_sample}`; }
function frameAddresses(f) { if (!f?.fcs_ok) return {}; return f.addresses && typeof f.addresses === 'object' ? f.addresses : {addr1: f.addr1, addr2: f.addr2, addr3: f.addr3}; }
function readable(v) { return v === null || v === undefined || v === '' ? 'Unavailable' : String(v); }
function evmText(v) { return typeof v === 'number' && Number.isFinite(v) ? `${v.toFixed(2)} %` : 'Unavailable'; }
function modulationRate(f) {
  const mod = f?.modulation ? String(f.modulation) : '';
  const rate = Number.isFinite(f?.rate_mbps) ? `${f.rate_mbps} Mb/s` : '';
  return [mod, rate].filter(Boolean).join(' · ') || 'Unavailable';
}
function normalizeMac(s) {
  const input = s.trim().toLowerCase();
  if (!input) return '';
  const compact = input.replace(/[:-]/g, '');
  if (!/^[0-9a-f]{12}$/.test(compact)) return null;
  return compact.match(/../g).join(':');
}
function matchesFilters(frame) {
  const addr = frameAddresses(frame);
  if (state.fcsOnly && !frame.fcs_ok) return false;
  return Object.entries(state.filters).every(([key, value]) => !value || (normalizeMac(addr[key] || '') === value));
}
function filtersChanged() {
  if (state.liveCandidate && !matchesFilters(state.liveCandidate)) state.liveCandidate = null;
  renderFrames(); renderConstellation(); renderChannelResponse();
}
function onFrames(d) {
  const frames = Array.isArray(d) ? d : (d?.frames || []);
  const byId = new Map(state.frames.map((f) => [frameId(f), f]));
  const verified = new Map(state.verifiedFrames.map((f) => [frameId(f), f]));
  for (const f of frames) { byId.set(frameId(f), f); if (f.fcs_ok) verified.set(frameId(f), f); }
  state.frames = [...byId.values()].slice(-200);
  state.verifiedFrames = [...verified.values()].slice(-100);
  if (d?.history !== true) {
    const usable = [...frames].reverse().find(f => f.fcs_ok === true && matchesFilters(f));
    if (usable) { state.liveCandidate = usable; state.liveCandidateAt = performance.now(); }
  }
  if (state.selectedFrame) state.selectedFrame = verified.get(frameId(state.selectedFrame)) || byId.get(frameId(state.selectedFrame)) || state.selectedFrame;
  renderFrames();
  renderConstellation(); renderChannelResponse();
}
function visibleFrames() {
  const verifiedIds = new Set(state.verifiedFrames.map(frameId));
  const all = [...state.verifiedFrames.slice().reverse(), ...state.frames.slice().reverse().filter(f => !verifiedIds.has(frameId(f)))];
  return all.filter(matchesFilters);
}
function selectFrame(f) { state.selectedFrame = f; selectPoints('frame'); renderFrames(); }
function renderFrames() {
  const list = visibleFrames();
  const allCount = state.frames.length + state.verifiedFrames.length;
  $('#frame-count').textContent = `${state.verifiedFrames.length} FCS verified · ${list.length} shown`;
  const body = $('#frames-body'); body.replaceChildren();
  for (const f of list) {
    const tr = document.createElement('tr');
    if (state.selectedFrame && frameId(state.selectedFrame) === frameId(f)) tr.classList.add('selected');
    const a = frameAddresses(f);
    for (const value of [f.capture_id, f.start_sample, readable(f.phy), modulationRate(f), readable(a.addr3), f.status || 'Candidate']) {
      const td = document.createElement('td'); td.textContent = String(value); tr.appendChild(td);
    }
    tr.onclick = () => { $('#frames-scroll').focus(); selectFrame(f); };
    body.appendChild(tr);
  }
  $('#frames-empty').hidden = !!list.length;
  if (!list.length) $('#frames-empty').textContent = allCount ? 'No frames match the active filters.' : 'Listening for candidates…';
  const f = state.selectedFrame || state.liveCandidate, a = f ? frameAddresses(f) : {};
  $('#return-live').hidden = !state.selectedFrame;
  $('#frame-title').textContent = f ? `Capture ${f.capture_id} · sample ${f.start_sample}` : 'Choose a candidate';
  $('#frame-phy').textContent = f ? readable(f.phy) : '—';
  $('#frame-modulation').textContent = f ? modulationRate(f) : '—';
  for (const key of ['addr1', 'addr2', 'addr3']) $(`#frame-${key}`).textContent = f ? readable(a[key]) : '—';
  $('#frame-evm').textContent = f ? evmText(f.evm_percent) : 'Unavailable';
  $('#frame-status').textContent = f ? [f.reason || f.status || 'Preamble candidate; frame not decoded.', f.fcs_ok ? f.evm_warning : f.training_warning].filter(Boolean).join(' ') : 'Decoded frame fields appear when available.';
  $('#frame-mpdu').textContent = f?.mpdu_hex || 'Not decoded';
  $('#frame-gain').textContent = Number.isFinite(f?.gain_index) ? `${f.gain_index} (${f.agc === true ? 'AGC' : f.agc === false ? 'manual' : 'mode unknown'}; capture start)` : 'Unavailable';
  $('#frame-dbm').textContent = Number.isFinite(f?.frame_mean_dbm) ? `${f.frame_mean_dbm.toFixed(2)} dBm` : 'Unavailable';
  for (const [id, key] of [['peak', 'frame_peak_dbfs'], ['mean', 'frame_mean_dbfs']]) $(`#frame-${id}`).textContent = Number.isFinite(f?.[key]) ? `${f[key].toFixed(2)} dBFS` : 'Unavailable';
  const fields = ['frame_type', 'frame_subtype', 'to_ds', 'from_ds', 'protected', 'sequence', 'fragment', 'header_length', 'addr4'];
  $('#frame-decoded').textContent = f?.fcs_ok ? fields.filter(k => f[k] !== undefined && f[k] !== null).map(k => `${k.replaceAll('_', ' ')}: ${f[k]}`).join('\n') || 'No additional fields' : 'Not decoded';
}
function freshLiveCandidate() {
  return state.liveCandidate;
}
function displayedCandidate() { return state.points === 'frame' ? state.selectedFrame : freshLiveCandidate(); }
function constellationGrid(x, w, h) {
  // A square plot keeps I and Q on the same scale, with room for tick labels.
  const size = Math.max(0, Math.min(w - 56, h - 42));
  const cx = (w + 20) / 2, cy = (h - 12) / 2, scale = size / 3;
  const left = cx - size / 2, right = cx + size / 2, top = cy - size / 2, bottom = cy + size / 2;
  x.font = '10px monospace';
  for (let n = -3; n <= 3; n++) {
    const value = n / 2, px = cx + value * scale, py = cy - value * scale;
    x.strokeStyle = n === 0 ? '#55717e' : '#294451';
    x.lineWidth = 1; x.beginPath(); x.moveTo(px, top); x.lineTo(px, bottom); x.moveTo(left, py); x.lineTo(right, py); x.stroke();
    x.fillStyle = '#9ab0ba'; x.textAlign = 'center'; x.textBaseline = 'top'; x.fillText(String(value), px, bottom + 5);
    x.textAlign = 'right'; x.textBaseline = 'middle'; x.fillText(String(value), left - 6, py);
  }
  return {cx, cy, scale};
}
function drawTraining(src) {
  const {x,w,h} = canvas('#training-canvas'); x.clearRect(0,0,w,h); const plot = constellationGrid(x,w,h);
  const points = src?.training_constellation || [];
  $('#training-empty').hidden = points.length > 0;
  $('#training-empty').textContent = src ? 'Training symbols unavailable for this frame' : 'Waiting for training symbols';
  x.fillStyle = '#36e5d2aa';
  for (const p of points) if (Number.isFinite(p[0]) && Number.isFinite(p[1])) { x.beginPath(); x.arc(plot.cx+p[0]*plot.scale,plot.cy-p[1]*plot.scale,2,0,7); x.fill(); }
}
function renderConstellation() {
  const isFrame = state.points === 'frame', snapshot = state.points === 'snapshot';
  const corrected = state.points === 'corrected' || isFrame;
  const candidate = displayedCandidate();
  const liveFresh = state.status?.mode === 'live' && ws?.readyState === WebSocket.OPEN && performance.now() - state.liveAt < 3000;
  const src = corrected ? candidate : snapshot ? state.snapshot : liveFresh ? state.live : null;
  const pts = corrected ? (src?.fcs_ok ? src?.constellation : []) : src?.points;
  const valid = Array.isArray(pts) ? pts.filter(p => Array.isArray(p) && Number.isFinite(p[0]) && Number.isFinite(p[1])) : [];
  $('#data-empty').hidden = valid.length > 0;
  $('#data-empty').textContent = src ? 'Decoded data symbols unavailable for this frame' : 'Waiting for decoded data symbols';
  const showPayload = corrected && !!src?.fcs_ok && valid.length > 0;
  const trainingEvm = corrected && !showPayload && valid.length > 0 && Number.isFinite(src?.training_evm_percent);
  const {x, w, h} = canvas('#constellation-canvas'); x.clearRect(0, 0, w, h); const plot = constellationGrid(x, w, h);
  if (valid.length) {
    x.fillStyle = isFrame ? '#ffb86b99' : '#46a4ff99';
    for (const [i, q] of valid) { x.beginPath(); x.arc(plot.cx + i * plot.scale, plot.cy - q * plot.scale, 1.8, 0, 7); x.fill(); }
  }
  drawTraining(src);
  $('#c-kind').textContent = isFrame ? 'SELECTED FRAME' : corrected ? (src ? `LIVE FRAME · CAPTURE ${src.capture_id}` : 'LIVE · WAITING FOR COMPLETE FRAME') : snapshot ? `SNAPSHOT #${src?.capture_id ?? '—'}` : 'LIVE RAW IQ';
  $('#c-title').textContent = showPayload ? 'Decoded data-field constellation' : corrected ? 'Corrected constellation' : snapshot ? 'Frozen raw IQ' : 'Raw baseband IQ';
  const trainingName = src?.training_kind || (/ofdm/i.test(src?.phy || '') || /ofdm/i.test(src?.detection || '') ? 'L-LTF' : 'Training');
  const phyText = [src?.phy, modulationRate(src)].filter(x => x && x !== 'Unavailable').join(' · ');
  $('#c-description').textContent = showPayload ? `Corrected data symbols for an FCS-verified${phyText ? ` ${phyText}` : ''} frame.` : corrected ? (valid.length ? `Estimated equalized ${trainingName} training symbols for an unverified preamble candidate. These are not decoded payload symbols.` : 'No current corrected constellation is available. Waiting for a usable candidate.') : src ? 'Samples are shown before frame synchronization or equalization. They are not demodulated symbols.' : 'No current raw IQ burst is available.';
  $('#c-points').textContent = valid.length || '—';
  $('#c-freq').textContent = src?.frequency_hz ? mhz(src.frequency_hz) : '—';
  $('#c-rate').textContent = src?.sample_rate_hz ? `${src.sample_rate_hz / 1e6} MS/s` : '—';
  $('#c-evm-label').textContent = trainingEvm ? `${trainingName} EVM (estimate)` : 'Data-field EVM (estimate)';
  $('#c-evm').textContent = corrected ? evmText(trainingEvm ? src.training_evm_percent : src?.evm_percent) : 'Unavailable';
  $('#c-evm-note').textContent = showPayload ? (src?.evm_warning || src?.evm_scope || 'Data-field EVM estimate; not standards-calibrated.') : corrected && src?.training_warning ? src.training_warning : trainingEvm ? `${trainingName} EVM estimate; not payload EVM.` : 'EVM requires synchronized, equalized symbols and a known reference.';
}
function renderChannelResponse() {
  const candidate = displayedCandidate();
  if (state.framePlot !== 'channel') { renderFrameWaveform(candidate); return; }
  $('#channel-note').textContent = 'Relative channel magnitude; gain is not calibrated.';
  $('#plot-scale').textContent = '−26 subcarriers                                 0                                 +26 subcarriers';
  const isOfdm = /ofdm/i.test(candidate?.phy || '') || /ofdm/i.test(candidate?.detection || '');
  const values = (isOfdm && Array.isArray(candidate?.channel_estimate) ? candidate.channel_estimate : []).filter(p => Array.isArray(p) && p.length >= 3 && p.every(Number.isFinite)).sort((a,b) => a[0] - b[0]);
  const {x, w, h} = canvas('#channel-canvas'); x.clearRect(0, 0, w, h); grid(x, w, h, 13, 4);
  if (!values.length) { $('#channel-meta').textContent = candidate && !isOfdm ? 'No OFDM subcarrier estimate for this PHY' : 'No current channel estimate'; return; }
  const magnitude = values.map(p => Math.hypot(p[1], p[2]));
  const peak = Math.max(...magnitude);
  if (!(peak > 0)) { $('#channel-meta').textContent = 'No usable channel estimate'; return; }
  const db = magnitude.map(v => Math.max(-40, 20 * Math.log10(Math.max(v / peak, 1e-4))));
  const pad=16, X=k => pad + (k + 26) / 52 * (w - 2 * pad), Y=v => 8 + (-v / 40) * (h - 20);
  trace(x, values.map(p=>p[0]), db, X, Y, h, '#36e5d2');
  x.fillStyle = '#7f97a3'; x.font = '11px monospace'; x.fillText('0 dB', 5, 12); x.fillText('−40 dB', 5, h - 6);
  const time = Number.isFinite(candidate.timestamp_us) ? ` · device trigger ~${candidate.timestamp_us} µs (u32)` : '';
  $('#channel-meta').textContent = `${state.points === 'frame' ? 'Selected' : 'Live'} candidate · capture ${candidate.capture_id} · sample ${candidate.start_sample}${time} · ${values.length} carriers`;
}
function renderFrameWaveform(frame) {
  const fft = state.framePlot === 'fft', values = fft ? frame?.frame_spectrum_db : frame?.frame_envelope_db;
  const {x,w,h} = canvas('#channel-canvas'); x.clearRect(0,0,w,h); grid(x,w,h);
  $('#channel-note').textContent = fft ? 'Mean spectral power across the complete decoded frame · dBFS (relative).' : 'Peak power per time bucket · dBFS.';
  $('#plot-scale').textContent = fft ? `−${(frame?.sample_rate_hz || 0)/2e6} MHz                                +${(frame?.sample_rate_hz || 0)/2e6} MHz` : `0 µs                                ${frame?.frame_duration_us ?? '—'} µs`;
  if (!Array.isArray(values) || values.length < 2) { $('#channel-meta').textContent = 'Complete frame waveform unavailable'; return; }
  const min = Math.floor(Math.min(...values)/10)*10, max = Math.max(min+10, Math.ceil(Math.max(...values)/10)*10);
  trace(x, values.map((_,i)=>i), values, i=>i/(values.length-1)*w, v=>h-(v-min)/(max-min)*h, h, '#36e5d2'); yLabels(x,h,min,max);
  $('#channel-meta').textContent = `Capture ${frame.capture_id} · ${frame.frame_duration_us} µs`;
}
function selectPoints(which) { state.points = which; $$('[data-points]').forEach((b) => b.classList.toggle('active', b.dataset.points === which)); renderConstellation(); renderChannelResponse(); }

function renderPower(d) {
  const p = d.power;
  $('#power-value').textContent = p.mean_dbfs.toFixed(1); $('#p-peak').textContent = `${p.peak_dbfs.toFixed(1)} dBFS`;
  $('#p-dc').textContent = `${p.dc_dbfs.toFixed(1)} dBFS`; $('#p-clip').textContent = `${(p.clipped * 100).toFixed(2)} %`;
  $('#p-gain').textContent = `${d.gain_index}${d.agc ? ' (AGC)' : ' (manual)'}`;
  $('#p-tone').textContent = mhz(p.tone_hz); $('#p-tone-db').textContent = calibrationMatches(d) ? `${(p.tone_dbfs + calibrationOffset(p.tone_hz)).toFixed(1)} dBm` : `${p.tone_dbfs.toFixed(1)} dBFS`;
  const g = canvas('#gauge-canvas'); g.x.clearRect(0, 0, g.w, g.h);
  const t = Math.max(0, Math.min(1, (p.mean_dbfs + 80) / 80)), cx = g.w / 2, cy = g.h * .86, r = Math.min(g.w * .38, g.h * .7);
  g.x.lineWidth = 16; g.x.lineCap = 'round'; g.x.strokeStyle = '#1c2d37'; g.x.beginPath(); g.x.arc(cx, cy, r, Math.PI, 2 * Math.PI); g.x.stroke();
  const grad = g.x.createLinearGradient(cx - r, 0, cx + r, 0); grad.addColorStop(0, '#46a4ff'); grad.addColorStop(.7, '#36e5d2'); grad.addColorStop(1, '#ff6477');
  g.x.strokeStyle = grad; g.x.beginPath(); g.x.arc(cx, cy, r, Math.PI, Math.PI + Math.PI * t); g.x.stroke();
  const {x, w, h} = canvas('#power-canvas'); x.clearRect(0, 0, w, h); grid(x, w, h);
  const hist = state.history; if (hist.length < 2) return;
  const all = hist.flatMap((v) => [v.mean, v.peak]), min = Math.floor(Math.min(...all) / 5) * 5 - 5, max = Math.ceil(Math.max(...all) / 5) * 5 + 5;
  const X = (i) => i / (hist.length - 1) * w, Y = (v) => h - (v - min) / (max - min) * h;
  trace(x, hist.map((_, i) => i), hist.map((v) => v.peak), X, Y, h, '#ff647788');
  trace(x, hist.map((_, i) => i), hist.map((v) => v.mean), X, Y, h, '#36e5d2');
  yLabels(x, h, min, max);
}

function renderSweep() {
  const {x, w, h} = canvas('#sweep-canvas'); x.clearRect(0, 0, w, h); grid(x, w, h, 10, 8);
  const tiles = state.sweep.tiles.filter(Boolean); if (!tiles.length) return;
  const fs = [], vs = [];
  for (const t of tiles) t.bins.forEach((v, j) => { fs.push(t.start_hz + (j + .5) * t.bin_hz); vs.push(v); });
  const [lo, hi] = state.sweep.range ?? [fs[0], fs[fs.length - 1]];
  const min = Math.floor(Math.min(...vs) / 10) * 10 - 5, max = Math.ceil(Math.max(...vs) / 10) * 10 + 5;
  trace(x, fs, vs, (f) => (f - lo) / (hi - lo) * w, (v) => h - (v - min) / (max - min) * h, h, '#36e5d2', '#36e5d222');
  yLabels(x, h, min, max);
}

// --- controls ---------------------------------------------------------------------
$$('.nav').forEach((b) => b.onclick = () => { $$('.nav,.view').forEach((x) => x.classList.remove('active')); b.classList.add('active'); $(`#view-${b.dataset.view}`).classList.add('active'); const settings = $('#capture-settings'); if (b.dataset.view === 'live') $('#spectrum-settings').appendChild(settings); else if (b.dataset.view === 'calibration') $('#calibration-settings').appendChild(settings); else $('.capture-column').prepend(settings); renderAll(); savePreferences(); });
$$('[data-detector]').forEach((b) => b.onclick = () => { $$('[data-detector]').forEach((x) => x.classList.toggle('active', x === b)); state.peakHold = null; send({op: 'config', detector: b.dataset.detector}); savePreferences(); });
$$('[data-points]').forEach((b) => b.onclick = () => selectPoints(b.dataset.points));
for (const id of ['freq','rate','gain','gain-index','bw','n','bits']) $(`#${id}`).addEventListener('change', () => {
  const inputs = ['freq','rate','gain-index','bw','n','bits'].map(key => $(`#${key}`));
  if (inputs.some(el => el.validity && !el.validity.valid)) { toast('Check capture settings'); return; }
  send(config());
});
$('#filter-fcs').addEventListener('change', () => { state.fcsOnly = $('#filter-fcs').checked; filtersChanged(); savePreferences(); });
$$('[data-frame-plot]').forEach(b => b.onclick = () => { state.framePlot = b.dataset.framePlot; $$('[data-frame-plot]').forEach(x => x.classList.toggle('active', x === b)); renderChannelResponse(); savePreferences(); });
$('#frames-scroll').addEventListener('keydown', e => {
  if (e.key !== 'ArrowUp' && e.key !== 'ArrowDown') return;
  const list = visibleFrames();
  if (!list.length) return;
  e.preventDefault();
  const index = state.selectedFrame ? list.findIndex(f => frameId(f) === frameId(state.selectedFrame)) : -1;
  const next = index < 0 ? 0 : Math.max(0, Math.min(list.length - 1, index + (e.key === 'ArrowDown' ? 1 : -1)));
  selectFrame(list[next]);
  $('#frames-body').children[next]?.scrollIntoView({block: 'nearest'});
});
$('#return-live').onclick = () => { state.selectedFrame = null; selectPoints('corrected'); renderFrames(); };
$('#run').onclick = () => send({op: 'live', on: state.status?.mode !== 'live'});
$('#snapshot').onclick = () => send({op: 'snapshot'});
for (const key of ['addr1', 'addr2', 'addr3']) {
  $(`#filter-${key}`).addEventListener('input', () => {
    const values = Object.fromEntries(['addr1', 'addr2', 'addr3'].map(k => [k, normalizeMac($(`#filter-${k}`).value)]));
    if (Object.values(values).some(v => v === null)) { savePreferences(); $('#filter-error').textContent = 'Use six hexadecimal octets, for example ca:fe:ba:be:00:01.'; return; }
    $('#filter-error').textContent = ''; state.filters = values; filtersChanged(); savePreferences();
  });
}
$('#clear-filters').onclick = () => { for (const key of ['addr1', 'addr2', 'addr3']) $(`#filter-${key}`).value = ''; state.filters = {addr1: '', addr2: '', addr3: ''}; state.fcsOnly = false; $('#filter-fcs').checked = false; $('#filter-error').textContent = ''; filtersChanged(); savePreferences(); };
$('#start-sweep').onclick = () => {
  if (state.status?.mode === 'sweep') { send({op: 'stop_sweep'}); return; }
  const start = +$('#sweep-start').value, stop = +$('#sweep-stop').value;
  if (start < 1850 || stop > 2850) toast('Note: outside the ESPARGOS-measured 1850–2850 MHz range');
  send({op: 'sweep', start_mhz: start, stop_mhz: stop, step_mhz: +$('#sweep-step').value, rate_hz: +$('#sweep-rate').value,
    fft: +$('#sweep-fft').value, dwell: +$('#sweep-dwell').value, detector: $('#detector').value, repeat: $('#sweep-repeat').checked});
};
function renderAll() { renderLive(); renderCalibration(); renderConstellation(); renderChannelResponse(); renderFrames(); if (state.live) renderPower(state.live); renderSweep(); }
addEventListener('resize', renderAll);
restorePreferences();
loadCalibration();
connect();
