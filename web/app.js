// Winject IQ Analyzer browser client. Talks to the host server (server.py)
// over one WebSocket at /ws; the ESP32U itself only streams samples.
const $ = (s) => document.querySelector(s), $$ = (s) => [...document.querySelectorAll(s)];
const state = {
  status: null, live: null, snapshot: null, points: 'live',
  history: [], sweep: {tiles: [], range: null}, peakHold: null,
};
let ws = null;

function connect() {
  ws = new WebSocket(`${location.protocol === 'https:' ? 'wss' : 'ws'}://${location.host}/ws`);
  ws.onopen = () => setStatus(true, 'Host connected');
  ws.onclose = () => { setStatus(false, 'Reconnecting…'); setTimeout(connect, 1500); };
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
  else if (m.event === 'snapshot') { state.snapshot = d; selectPoints('snapshot'); const a = $('#download'); a.href = d.download; a.hidden = false; toast(`Snapshot ${d.capture_id}: ${d.samples} samples`); }
  else if (m.event === 'sweep.start') { state.sweep = {tiles: [], range: [(d.start_mhz - d.step_mhz / 2) * 1e6, (d.stop_mhz + d.step_mhz / 2) * 1e6]}; $('#sweep-progress').textContent = `0 / ${d.steps}`; $('#scale-start').textContent = `${d.start_mhz} MHz`; $('#scale-stop').textContent = `${d.stop_mhz} MHz`; }
  else if (m.event === 'sweep.tile') { state.sweep.tiles[d.index] = d; $('#sweep-progress').textContent = `${d.index + 1} / ${d.total} · gain ${d.gain_index}`; renderSweep(); }
  else if (m.event === 'sweep.done') { $('#sweep-progress').textContent = `${d.state} · ${d.seconds} s`; toast(`Sweep ${d.state} in ${d.seconds} s`); }
  else if (m.event === 'error') toast(d.message);
}

function applyStatus(s) {
  const first = !state.status;
  state.status = s;
  const r = s.radio || {};
  setStatus(!r.error, r.error ? `Radio: ${r.error}` : `ESP32U ${s.radio_ip} · ${s.mode}`);
  $('#rate-metric').textContent = `${s.captures_per_s}/s`;
  $('#lost').textContent = `${s.lost}${s.errors ? ` · ${s.errors} err` : ''}`;
  $('#run').textContent = s.mode === 'live' ? 'Stop' : 'Run';
  $('#start-sweep').textContent = s.mode === 'sweep' ? 'Stop sweep' : 'Start sweep';
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
  state.history.push({t: performance.now(), mean: d.power.mean_dbfs, peak: d.power.peak_dbfs});
  state.history = state.history.slice(-300);
  $('#live-meta').textContent = `${mhz(d.frequency_hz)} · ${d.sample_rate_hz / 1e6} MS/s · ${d.samples} × IQ${d.bits} · gain ${d.gain_index}${d.agc ? ' (AGC)' : ''}`;
  renderLive(); renderPower(d); if (state.points === 'live') renderConstellation();
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
  const min = Math.floor(Math.min(...db) / 10) * 10 - 5, max = Math.max(0, Math.ceil(Math.max(...db) / 10) * 10 + 5);
  const X = (f) => (f - freqs[0]) / (freqs[n - 1] - freqs[0]) * w, Y = (v) => h - (v - min) / (max - min) * h;
  trace(x, freqs, db, X, Y, h, '#36e5d2', '#36e5d222');
  const t = d.power; x.fillStyle = '#ff6477'; x.beginPath(); x.arc(X(t.tone_hz), Y(t.tone_dbfs), 3.5, 0, 7); x.fill();
  yLabels(x, h, min, max);
  $('#live-lo').textContent = mhz(freqs[0]); $('#live-hi').textContent = mhz(freqs[n - 1]);
  const e = canvas('#live-envelope'); e.x.clearRect(0, 0, e.w, e.h); grid(e.x, e.w, e.h, 10, 3);
  const env = d.envelope, emin = -60, emax = 5, EY = (v) => e.h - (Math.max(emin, v) - emin) / (emax - emin) * e.h;
  trace(e.x, env.map((_, i) => i), env, (i) => i / (env.length - 1) * e.w, EY, e.h, '#46a4ff', '#46a4ff22');
  yLabels(e.x, e.h, emin, emax);
  $('#env-span').textContent = `${(d.samples / d.sample_rate_hz * 1e6).toFixed(0)} µs`;
}

function renderConstellation() {
  const src = state.points === 'snapshot' ? state.snapshot : state.live;
  const {x, w, h} = canvas('#constellation-canvas'); x.clearRect(0, 0, w, h); grid(x, w, h, 8, 8);
  x.strokeStyle = '#38505d'; x.beginPath(); x.moveTo(w / 2, 0); x.lineTo(w / 2, h); x.moveTo(0, h / 2); x.lineTo(w, h / 2); x.stroke();
  if (!src) return;
  const pts = src.points, peak = Math.max(1 / 128, ...pts.map(([i, q]) => Math.hypot(i, q))), k = 1 / peak;
  x.fillStyle = state.points === 'snapshot' ? '#ffb86b88' : '#46a4ff99';
  for (const [i, q] of pts) { x.beginPath(); x.arc(w / 2 + i * k * w * .45, h / 2 - q * k * h * .45, 1.8, 0, 7); x.fill(); }
  $('#c-kind').textContent = state.points === 'snapshot' ? `SNAPSHOT #${src.capture_id}` : 'LIVE';
  $('#c-title').textContent = state.points === 'snapshot' ? 'Frozen capture' : 'Raw baseband IQ';
  $('#c-points').textContent = pts.length; $('#c-freq').textContent = mhz(src.frequency_hz);
  $('#c-rate').textContent = `${src.sample_rate_hz / 1e6} MS/s`; $('#c-edge').textContent = `${(20 * Math.log10(peak)).toFixed(1)} dBFS`;
}
function selectPoints(which) { state.points = which; $$('[data-points]').forEach((b) => b.classList.toggle('active', b.dataset.points === which)); renderConstellation(); }

function renderPower(d) {
  const p = d.power;
  $('#power-value').textContent = p.mean_dbfs.toFixed(1); $('#p-peak').textContent = `${p.peak_dbfs.toFixed(1)} dBFS`;
  $('#p-dc').textContent = `${p.dc_dbfs.toFixed(1)} dBFS`; $('#p-clip').textContent = `${(p.clipped * 100).toFixed(2)} %`;
  $('#p-gain').textContent = `${d.gain_index}${d.agc ? ' (AGC)' : ' (manual)'}`;
  $('#p-tone').textContent = mhz(p.tone_hz); $('#p-tone-db').textContent = `${p.tone_dbfs.toFixed(1)} dBFS`;
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
$$('.nav').forEach((b) => b.onclick = () => { $$('.nav,.view').forEach((x) => x.classList.remove('active')); b.classList.add('active'); $(`#view-${b.dataset.view}`).classList.add('active'); renderAll(); });
$$('[data-detector]').forEach((b) => b.onclick = () => { $$('[data-detector]').forEach((x) => x.classList.toggle('active', x === b)); state.peakHold = null; send({op: 'config', detector: b.dataset.detector}); });
$$('[data-points]').forEach((b) => b.onclick = () => selectPoints(b.dataset.points));
$('#apply').onclick = () => { send(config()); toast('Applied'); };
$('#run').onclick = () => send({op: 'live', on: state.status?.mode !== 'live'});
$('#snapshot').onclick = () => send({op: 'snapshot'});
$('#start-sweep').onclick = () => {
  if (state.status?.mode === 'sweep') { send({op: 'stop_sweep'}); return; }
  const start = +$('#sweep-start').value, stop = +$('#sweep-stop').value;
  if (start < 1850 || stop > 2850) toast('Note: outside the ESPARGOS-measured 1850–2850 MHz range');
  send({op: 'sweep', start_mhz: start, stop_mhz: stop, step_mhz: +$('#sweep-step').value, rate_hz: +$('#sweep-rate').value,
    fft: +$('#sweep-fft').value, dwell: +$('#sweep-dwell').value, detector: $('#detector').value, repeat: $('#sweep-repeat').checked});
};
function renderAll() { renderLive(); renderConstellation(); if (state.live) renderPower(state.live); renderSweep(); }
addEventListener('resize', renderAll);
connect();
