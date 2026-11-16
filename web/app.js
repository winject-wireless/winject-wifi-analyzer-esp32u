// Winject IQ Analyzer browser client. Talks to the host server (server.py)
// over one WebSocket at /ws; the ESP32U itself only streams samples.
const $ = (s) => document.querySelector(s), $$ = (s) => [...document.querySelectorAll(s)];
const state = {
  status: null, live: null, liveAt: 0, snapshot: null, points: 'corrected',
  history: [], sweep: {}, peakHold: null,
  frames: [], verifiedFrames: [], selectedFrame: null, liveCandidate: null, liveCandidateAt: 0, filters: {addr1: '', addr2: '', addr3: ''}, fcsOnly: false, completeOnly: false, framePlot: 'fft', calibration: null,
};
let ws = null;
const preferencesKey = 'winject-analyzer-view-v1';
const sweepNumberFields = ['start', 'stop', 'bw', 'overlap', 'time', 'gain'];
const sweepValueFields = [...sweepNumberFields.map(name => `sweep-${name}`), 'sweep-bits', 'detector'];
function savePreferences() {
  const activeView = $('.nav.active');
  const activeDetector = $('[data-detector].active');
  const preferences = {
    view: activeView?.dataset.view, detector: activeDetector?.dataset.detector,
    framePlot: state.framePlot, fcsOnly: state.fcsOnly, completeOnly: state.completeOnly, filters: state.filters,
    sweepInputs: Object.fromEntries(sweepValueFields.map(id => [id, $(`#${id}`).value])),
    sweepRepeat: $('#sweep-repeat').checked,
    filterInputs: Object.fromEntries(['addr1', 'addr2', 'addr3'].map(k => [k, $(`#filter-${k}`).value])),
  };
  try { localStorage.setItem(preferencesKey, JSON.stringify(preferences)); } catch (_) { /* Storage may be unavailable. */ }
}
function restorePreferences() {
  let saved;
  try { saved = JSON.parse(localStorage.getItem(preferencesKey)); } catch (_) { return; }
  if (!saved || typeof saved !== 'object') return;
  for (const id of sweepValueFields) {
    if (typeof saved.sweepInputs?.[id] === 'string') $(`#${id}`).value = saved.sweepInputs[id];
  }
  $('#sweep-repeat').checked = saved.sweepRepeat === true;
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
  state.completeOnly = saved.completeOnly === true;
  $('#filter-complete').checked = state.completeOnly;
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
  else if (m.event === 'sweep.start') beginSweep(d);
  else if (m.event === 'sweep.tile') collectSweepPeek(d);
  else if (m.event === 'sweep.done') {
    if (d.job !== state.sweep.job) return;
    delete state.sweep.pending;
    delete state.sweep.radioSettings;
    $('#start-sweep').textContent = 'Play';
    renderSweepRadioSettings();
    $('#sweep-progress').textContent = `${d.state} · ${d.seconds} s`;
    toast(`Sweep ${d.state} in ${d.seconds} s`);
  }
  else if (m.event === 'calibration.progress') { $('#calibration-auto-cancel').hidden = false; $('#calibration-auto-progress').textContent = `Measuring gain ${d.gain} of ${d.end_gain ?? d.total - 1} · ${d.accepted} accepted · ${d.skipped} skipped`; }
  else if (m.event === 'calibration.done') {
    $('#calibration-auto-cancel').hidden = true;
    const points = Array.isArray(d.points) ? d.points : [];
    if (points.length) {
      if (!state.calibration) state.calibration = {version:1, points:[]};
      for (const point of points) state.calibration.points = state.calibration.points.filter(p => !(p.hz === point.hz && sameCalibrationSettings(p.settings || state.calibration.settings, point.settings)));
      state.calibration.points.push(...points); state.calibration.points.sort((a,b) => a.hz-b.hz);
      storeCalibration(); renderCalibrationTable(); renderLive(); renderCalibration();
    }
    $('#calibration-auto-progress').textContent = d.error ? `Calibration failed: ${d.error}` : d.cancelled ? 'Calibration cancelled.' : `Finished: ${points.length} gains calibrated, ${d.skipped.length} skipped (insufficient consistent captures).`;
  }
  else if (m.event === 'error') {
    const message = d.message?.includes('iq_sweep') && d.message.includes('dwell_ms=') && d.message.includes('EINVAL')
      ? 'Sweep rejected: analyzer firmware may need the timed sweep update.' : d.message;
    $('#calibration-auto-progress').textContent = message; $('#sweep-progress').textContent = message; toast(message);
  }
}

let radioAddressDirty = false;
$("#radio-address").addEventListener("input", () => { radioAddressDirty = true; });
function applyStatus(s) {
  const first = !state.status;
  state.status = s;
  if (radioAddressDirty && s.radio_ip === $("#radio-address").value.trim() && document.activeElement !== $("#radio-address")) radioAddressDirty = false;
  if (!radioAddressDirty && document.activeElement !== $("#radio-address")) $("#radio-address").value = s.radio_ip || "";
  const r = s.radio || {};
  setStatus(!r.error, r.error ? `Radio: ${r.error}` : `ESP32U ${s.radio_ip} · ${s.mode}`);
  $('#rate-metric').textContent = `${s.captures_per_s}/s`;
  $('#lost').textContent = `${s.lost}${s.errors ? ` · ${s.errors} err` : ''}`;
  $('#run').textContent = s.mode === 'live' ? 'Pause' : 'Play';
  if (s.mode === 'sweep' && s.sweep) beginSweep(s.sweep);
  $('#start-sweep').textContent = s.mode === 'sweep' ? 'Pause' : 'Play';
  if (state.points !== 'frame' && state.points !== 'snapshot') { renderConstellation(); renderChannelResponse(); }
  const detector = $('#detector-status');
  const sampleRates = Array.isArray(s.detector_sample_rates_hz) ? s.detector_sample_rates_hz.filter(Number.isFinite).map(r => `${r / 1e6} MS/s`).join(', ') : '';
  const formats = Array.isArray(s.decoder_formats) ? s.decoder_formats.filter(v => typeof v === 'string').join(', ') : '';
  const rates = Array.isArray(s.decoder_rates_mbps) ? s.decoder_rates_mbps.filter(Number.isFinite).join(', ') : '';
  const backend = typeof s.decoder_backend === 'string' && s.decoder_backend ? s.decoder_backend : '';
  const capability = [backend, formats, rates ? `${rates} Mb/s` : ''].filter(Boolean).join(' · ');
  detector.textContent = s.detector_error ? `Detector error: ${s.detector_error}` : s.decoder_error ? `Decoder error: ${s.decoder_error}` : !s.detector_supported ? `Frame detection unavailable at this sample rate${sampleRates ? `; supported: ${sampleRates}` : ''}. Raw IQ remains available.` : `Detector: ${s.detector_processed ?? 0} bursts, ${s.detector_dropped ?? 0} skipped · Decoder${capability ? ` (${capability})` : ''}: ${s.decoder_decoded ?? 0} FCS verified / ${s.decoder_attempted ?? 0} attempted, ${s.decoder_dropped ?? 0} skipped`;
  if (formats.includes('DSSS')) detector.textContent += s.dsss_error ? ` · DSSS error: ${s.dsss_error}` : ` · DSSS/CCK: ${s.dsss_decoded ?? 0} FCS verified / ${s.dsss_attempted ?? 0} bursts, ${s.dsss_dropped ?? 0} skipped`;


  if (r.gain_max) { $('#gain-index').max = r.gain_max; $('#sweep-gain').max = r.gain_max; }
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
  addWaterfallCapture(d); renderLive(); renderCalibration(); renderPower(d); if (state.points === 'raw' || state.points === 'corrected') { renderConstellation(); renderChannelResponse(); }
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
function spectrumAxisLabels(ctx, width, height, min, max, offset) {
  yLabels(ctx, height, min, max, 'dBFS');
  if (!Number.isFinite(offset)) return;
  ctx.fillStyle = '#7f97a3'; ctx.font = '11px monospace'; ctx.textAlign = 'right';
  for (let k = 0; k <= 4; k++) {
    const value = max - (max - min) * k / 4 + offset;
    ctx.fillText(`${value.toFixed(0)} dBm`, width - 6, Math.max(12, k * height / 4 - 4));
  }
  ctx.textAlign = 'start';
}
function spectrumFrequencyLabels(ctx, width, height, firstHz, lastHz) {
  ctx.fillStyle = '#a6bbc5'; ctx.font = width < 500 ? '8px monospace' : '10px monospace';
  ctx.textAlign = 'center'; ctx.textBaseline = 'bottom';
  const decimals = width < 500 ? 1 : 2;
  for (let i = 1; i < 10; i++) {
    const hz = firstHz + (lastHz - firstHz) * i / 10;
    ctx.fillText((hz / 1e6).toFixed(decimals), width * i / 10, height - 4);
  }
  ctx.textAlign = 'start'; ctx.textBaseline = 'alphabetic';
}
function trace(ctx, xs, ys, X, Y, h, color, fill) {
  ctx.strokeStyle = color; ctx.lineWidth = 1.3; ctx.beginPath();
  xs.forEach((f, i) => i ? ctx.lineTo(X(f), Y(ys[i])) : ctx.moveTo(X(f), Y(ys[i])));
  if (fill) { ctx.stroke(); ctx.lineTo(X(xs[xs.length - 1]), h); ctx.lineTo(X(xs[0]), h); ctx.closePath(); ctx.fillStyle = fill; ctx.fill(); } else ctx.stroke();
}

// Keep spectrum axes steady through brief peaks; expand immediately to avoid clipping.
const spectrumAxes = {};
function spectrumScale(name, values, unit, capture) {
  const low = Math.min(...values), high = Math.max(...values), now = performance.now();
  const key = `${unit}:${capture.frequency_hz}:${capture.sample_rate_hz}:${capture.gain_index}:${capture.agc}`;
  const targetMin = Math.floor(low / 10) * 10 - 5;
  const targetMax = Math.max(Math.ceil(high / 10) * 10 + 5, targetMin + 10);
  let axis = spectrumAxes[name];
  if (!axis || axis.key !== key) axis = spectrumAxes[name] = {key, min:targetMin, max:targetMax, changed:now};
  else if (low < axis.min || high > axis.max) {
    axis.min = Math.min(axis.min, targetMin); axis.max = Math.max(axis.max, targetMax); axis.changed = now;
  } else if (now - axis.changed >= 10000 && (targetMin > axis.min || targetMax < axis.max)) {
    axis.min = targetMin; axis.max = targetMax; axis.changed = now;
  }
  return axis;
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
  const {min, max} = spectrumScale('live', db, 'dBFS', d);
  const axisOffset = calibrated ? calibrationOffset(d.frequency_hz, offsets) : null;
  $('#live-axis-note').textContent = calibrationAxisNote(d, calibrated);
  $('#live-axis-note').title = $('#live-axis-note').textContent;
  const X = (f) => (f - freqs[0]) / (freqs[n - 1] - freqs[0]) * w, Y = (v) => h - (v - min) / (max - min) * h;
  trace(x, freqs, db, X, Y, h, '#36e5d2', '#36e5d222');
  const t = d.power; x.fillStyle = '#ff6477'; x.beginPath(); x.arc(X(t.tone_hz), Y(t.tone_dbfs), 3.5, 0, 7); x.fill();
  spectrumAxisLabels(x, w, h, min, max, axisOffset);
  spectrumFrequencyLabels(x, w, h, freqs[0], freqs[n - 1]);
  $('#live-lo').textContent = mhz(freqs[0]); $('#live-hi').textContent = mhz(freqs[n - 1]);

}

// A fixed dBFS scale keeps colors comparable across captures. Each row retains
// its frequency geometry so history can be viewed after retuning.
const waterfall = {rows: [], key: null, rowHeight: 3, limit: 600, scroll: '#waterfall-scroll', space: '#waterfall-space', canvas: '#live-waterfall'};
const sweepWaterfall = {rows: [], key: null, rowHeight: 3, limit: 600, scroll: '#sweep-waterfall-scroll', space: '#sweep-waterfall-space', canvas: '#sweep-waterfall'};
function waterfallColor(value) {
  const t = Math.max(0, Math.min(1, (value + 100) / 100));
  const stops = [[0, 8, 40], [0, 65, 210], [0, 210, 220], [255, 225, 0], [235, 30, 20]];
  const pos = t * (stops.length - 1), i = Math.min(stops.length - 2, Math.floor(pos)), f = pos - i;
  return stops[i].map((v, c) => Math.round(v + (stops[i + 1][c] - v) * f));
}
const waterfallPalette = Array.from({length: 101}, (_, i) => waterfallColor(i - 100));
function addWaterfallCapture(d, target = waterfall) {
  const s = d.spectrum;
  if (!s?.db?.length) return;
  const scroll = $(target.scroll), following = scroll.scrollTop <= 1;
  target.geometry = {start: s.start_hz, bin: s.bin_hz, length: s.db.length};
  target.rows.unshift({values: s.db.slice(), start: s.start_hz, bin: s.bin_hz});
  if (target.rows.length > target.limit) target.rows.pop();
  $(target.space).style.height = `${target.rows.length * target.rowHeight}px`;
  if (!following && target.rows.length > 1) scroll.scrollTop += target.rowHeight;
  renderWaterfall(target);
}
function renderWaterfall(target = waterfall) {
  const scroll = $(target.scroll), c = $(target.canvas);
  const width = Math.round(scroll.clientWidth), height = Math.round(scroll.clientHeight);
  if (!width || !height || !target.rows.length) return;
  // The bitmap covers only the visible viewport, independent of history length.
  c.width = width; c.height = height;
  c.style.height = `${height}px`;
  const x = c.getContext('2d'), img = x.createImageData(width, height);
  for (let y = 0; y < height; y++) {
    const row = target.rows[Math.floor((scroll.scrollTop + y) / target.rowHeight)];
    if (!row) continue;
    for (let px = 0; px < width; px++) {
      const geometry = target.geometry;
      const frequency = geometry.start + (width > 1 ? px / (width - 1) : 0) * (geometry.length - 1) * geometry.bin;
      const source = Math.round((frequency - row.start) / row.bin);
      const value = source >= 0 && source < row.values.length ? row.values[source] : NaN;
      const color = waterfallPalette[Math.max(0, Math.min(100, Math.round(Number.isFinite(value) ? value + 100 : 0)))];
      const i = (y * width + px) * 4;
      img.data[i] = color[0]; img.data[i + 1] = color[1]; img.data[i + 2] = color[2]; img.data[i + 3] = 255;
    }
  }
  x.putImageData(img, 0, 0);
}
$('#waterfall-scroll').addEventListener('scroll', () => renderWaterfall());
$('#sweep-waterfall-scroll').addEventListener('scroll', () => renderWaterfall(sweepWaterfall));

// Calibration offsets convert a spectrum bin from dBFS to known input dBm.
const calibrationKey = 'winject-spectrum-calibration-v1';
function calibrationSettings(d) {
  const c = state.status?.config;
  return c && d && Number.isFinite(d.gain_index) ? {gain: Number(d.gain_index), rate_hz: d.sample_rate_hz, nfft: d.spectrum.db.length, samples: d.samples, detector: c.detector, bw_mhz: Number(c.bw_mhz), bits: d.bits} : null;
}
function sameCalibrationSettings(a,b) { return !!(a && b && ['gain','bits'].every(k => a[k] === b[k])); }
function matchingCalibrationSettings(settings) { return (state.calibration?.points || []).filter(p => sameCalibrationSettings(settings, p.settings || state.calibration.settings)); }
function matchingCalibrationPoints(d) { return matchingCalibrationSettings(calibrationSettings(d)); }
function calibrationMatches(d) { return matchingCalibrationPoints(d).length > 0; }
function calibrationMissingReason(settings, agc) {
  if (!state.calibration?.points?.length) return 'No calibration saved in this browser.';
  if (!settings) return 'Capture settings are unavailable.';
  const fields = [['bits', 'IQ bit depth']];
  const gainsForSettings = new Set();
  let closest = null;
  for (const point of state.calibration.points) {
    const saved = point.settings || state.calibration.settings;
    const differences = fields.filter(([key]) => settings[key] !== saved[key]);
    if (!differences.length) gainsForSettings.add(saved.gain);
    if (saved.gain !== settings.gain) differences.push(['gain', 'Gain']);
    if (!closest || differences.length < closest.differences.length) closest = {saved, differences};
  }
  if (!gainsForSettings.size) {
    const changes = closest.differences.map(([key, label, format = String]) =>
      `${label} ${format(settings[key])} → ${format(closest.saved[key])}`);
    return `Closest saved settings require ${changes.join(' · ')}.`;
  }
  const gains = [...gainsForSettings].sort((a,b)=>a-b);
  const shown = gains.slice(0, 8).join(', '), remaining = gains.length > 8 ? ` +${gains.length - 8} more` : '';
  return `Gain ${settings.gain} is uncalibrated${agc ? ' (AGC varies)' : ''}. Calibrated gains: ${shown}${remaining}.`;
}
function calibrationAxisNote(d, active) {
  if (active) return d?.agc ? 'dBm uses capture-start gain; AGC may change during capture.' : 'dBm scale uses the center-frequency calibration.';
  return `dBm unavailable: ${calibrationMissingReason(calibrationSettings(d), d?.agc)}`;
}
function validCalibration(c) {
  const validSettings = s => s && Number.isFinite(s.gain) && [8,10].includes(s.bits);
  return c?.version === 1 && Array.isArray(c.points) && c.points.length > 0 && c.points.length <= 1000 && c.points.every(p => validSettings(p.settings || c.settings) && Number.isFinite(p.hz) && p.hz > 0 && Number.isFinite(p.measured_dbfs) && Number.isFinite(p.actual_dbm));
}
function storeCalibration() { try { if (state.calibration) localStorage.setItem(calibrationKey, JSON.stringify(state.calibration)); else localStorage.removeItem(calibrationKey); } catch (_) {} }
function loadCalibration() { try { const c = JSON.parse(localStorage.getItem(calibrationKey)); if (validCalibration(c)) { c.points.sort((a,b) => a.hz-b.hz); state.calibration = c; } } catch (_) {} renderCalibrationTable(); }
function calibrationOffsets(settings=calibrationSettings(state.live)) {
  const groups = new Map();
  for (const p of matchingCalibrationSettings(settings)) { const group=groups.get(p.hz) || []; group.push(p.actual_dbm-p.measured_dbfs); groups.set(p.hz,group); }
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
  renderCalibrationStatus(state.live);
  renderFrames(); renderChannelResponse();
}
function renderCalibrationStatus(d) {
  const status = $('#calibration-status');
  const active = calibrationMatches(d);
  const gain = Number.isFinite(d?.gain_index) ? ` · gain ${d.gain_index}` : '';
  const reason = state.calibration && !active ? calibrationMissingReason(calibrationSettings(d), d?.agc) : '';
  status.textContent = !state.calibration ? 'No calibration' : active ? `Calibration active${gain}` : `Calibration inactive${gain}. ${reason}`;
  status.title = reason || calibrationAxisNote(d, active);
  return active;
}
function renderCalibration() {
  const d = state.live;
  const active = renderCalibrationStatus(d);
  if (!d?.spectrum?.db?.length) return;
  const {x,w,h} = canvas('#calibration-spectrum'); if (!w || !h) return;
  x.clearRect(0,0,w,h); grid(x,w,h);
  const s=d.spectrum, n=s.db.length, freq=i=>s.start_hz+i*s.bin_hz, raw=s.db;
  const offsets=active ? calibrationOffsets() : [];
  const axisOffset=active ? calibrationOffset(d.frequency_hz, offsets) : null;
  const {min,max}=spectrumScale('calibration', raw, 'dBFS', d), X=i=>i/(n-1)*w, Y=v=>h-(v-min)/(max-min)*h;
  trace(x, raw.map((_,i)=>i), raw, X, Y, h, '#36e5d2', '#36e5d222');
  spectrumAxisLabels(x,w,h,min,max,axisOffset);
  spectrumFrequencyLabels(x,w,h,freq(0),freq(n-1));
  let peak=0; for (let i=1;i<n;i++) if (raw[i]>raw[peak]) peak=i;
  x.fillStyle='#ff6477'; x.beginPath(); x.arc(X(peak),Y(raw[peak]),4,0,7); x.fill();
  x.font='12px monospace'; x.textAlign='center'; x.textBaseline='top'; x.fillStyle='#e8f0f4';
  const nearCenter = Math.abs(freq(peak) - d.frequency_hz) < 500000;
  x.fillText(`${nearCenter ? 'Center peak (may be DC spur)' : 'Peak'} ${mhz(freq(peak))} · ${raw[peak].toFixed(1)} dBFS`,w/2,10);
  x.fillText(active ? `Calibrated ${(raw[peak]+calibrationOffset(freq(peak),offsets)).toFixed(1)} dBm` : 'Calibrated dBm unavailable for current settings',w/2,27);
  x.textAlign='start'; x.textBaseline='alphabetic';
  for (const p of matchingCalibrationPoints(d)) { const i=(p.hz-s.start_hz)/s.bin_hz; if (i>=0 && i<n) { x.fillStyle='#ffbf5f'; x.beginPath(); x.arc(X(i),Y(p.measured_dbfs),5,0,7); x.fill(); } }
  $('#calibration-axis-note').textContent=calibrationAxisNote(d, active);
  $('#calibration-axis-note').title=$('#calibration-axis-note').textContent;
  $('#calibration-lo').textContent=mhz(freq(0)); $('#calibration-hi').textContent=mhz(freq(n-1));
}
$('#calibration-auto').onclick = () => {
  const s = state.live?.spectrum;
  if (state.status?.mode !== 'live' || !s?.db?.length) { toast('Start live capture to calibrate'); return; }
  $('#calibration-auto-frequency').value = '';
  const gainMax = Number(state.status?.radio?.gain_max);
  $('#calibration-gain-start').value = '0';
  $('#calibration-gain-end').value = Number.isInteger(gainMax) && gainMax >= 0 ? String(gainMax) : '';
  $('#calibration-auto-tone').textContent = 'Enter a frequency, click the tone, or use the current peak. The selected frequency stays fixed during the sweep.';
  $('#calibration-auto-power').value = ''; $('#calibration-auto-form').hidden = false; $('#calibration-auto-power').focus();
};
$('#calibration-auto-close').onclick = () => { $('#calibration-auto-form').hidden = true; };
$('#calibration-use-peak').onclick = () => {
  const s = state.live?.spectrum;
  if (state.status?.mode !== 'live' || !s?.db?.length) { toast('Start live capture to select a peak'); return; }
  const n = s.db.length, middle = Math.floor(n / 2);
  const guard = Math.max(4, Math.ceil(500000 / s.bin_hz));
  let peak = -1;
  for (let i = 4; i < n - 4; i++) {
    if (Math.abs(i - middle) <= guard || !Number.isFinite(s.db[i])) continue;
    if (peak < 0 || s.db[i] > s.db[peak]) peak = i;
  }
  if (peak < 0) { $('#calibration-auto-tone').textContent = 'No usable peak outside the center spur and spectrum edges.'; return; }
  const tone = s.start_hz + peak * s.bin_hz;
  $('#calibration-auto-frequency').value = String(tone / 1e6);
  $('#calibration-auto-tone').textContent = `Selected peak ${mhz(tone)} · ${s.db[peak].toFixed(1)} dBFS. Confirm this is the reference tone; frequency stays fixed during the sweep.`;
};
$('#calibration-auto-form').onsubmit = e => {
  e.preventDefault(); const value = $('#calibration-auto-power').value, frequency = $('#calibration-auto-frequency').value;
  const s = state.live?.spectrum, tone = Number(frequency) * 1e6;
  if (!value.trim() || !Number.isFinite(Number(value)) || !frequency.trim() || !Number.isFinite(tone) || !s?.db?.length) { $('#calibration-auto-tone').textContent = 'Enter a valid tone power and frequency with live capture running.'; return; }
  const index = Math.round((tone - s.start_hz) / s.bin_hz), middle = Math.floor(s.db.length / 2);
  if (index < 4 || index >= s.db.length - 4 || Math.abs(index - middle) <= 4) { $('#calibration-auto-tone').textContent = 'Tone must be within the spectrum and away from the center frequency (DC).'; return; }
  if (ws?.readyState !== WebSocket.OPEN) { toast('Host disconnected'); return; }
  const first = $('#calibration-gain-start').value, last = $('#calibration-gain-end').value;
  const startGain = Number(first), endGain = Number(last), gainMax = Number(state.status?.radio?.gain_max);
  if (!first.trim() || !last.trim() || !Number.isInteger(startGain) || !Number.isInteger(endGain) || startGain < 0 || endGain < startGain || !Number.isInteger(gainMax) || endGain > gainMax) { $('#calibration-auto-tone').textContent = `Choose a gain range from 0 to ${Number.isInteger(gainMax) ? gainMax : 'the receiver limit'}, with start no greater than end.`; return; }
  send({op:'calibration.start', power_dbm:Number(value), tone_hz:tone, start_gain:startGain, end_gain:endGain});
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
  const i = Math.max(0,Math.min(s.db.length-1,Math.round((e.clientX-r.left)/r.width*(s.db.length-1))));
  if (!$('#calibration-auto-form').hidden) {
    const tone = s.start_hz + i * s.bin_hz;
    $('#calibration-auto-frequency').value = String(tone / 1e6);
    $('#calibration-auto-tone').textContent = `Selected ${mhz(tone)} · ${s.db[i].toFixed(1)} dBFS. Confirm this is the reference tone.`;
    return;
  }
  addCalibrationPoint(i);
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
// Partial frames carry addresses decoded from the captured header (no FCS: unverified).
function frameAddresses(f) { if (!f?.fcs_ok && f?.header_verified !== false) return {}; return f.addresses && typeof f.addresses === 'object' ? f.addresses : {addr1: f.addr1, addr2: f.addr2, addr3: f.addr3}; }
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
  // Complete frames only: hide frames cut off by the end of the capture.
  if (state.completeOnly && frame.partial === true) return false;
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
    const usable = [...frames].reverse().find(f => hasDataSymbols(f) && matchesFilters(f));
    if (usable) { state.liveCandidate = usable; state.liveCandidateAt = performance.now(); }
  }
  if (state.selectedFrame) state.selectedFrame = verified.get(frameId(state.selectedFrame)) || byId.get(frameId(state.selectedFrame)) || state.selectedFrame;
  renderFrames();
  renderConstellation(); renderChannelResponse();
}
// Selecting a frame holds the view: the table keeps the rows it showed at that moment
// while capture continues and new frames are collected underneath. Clearing the
// selection (Resume live) returns to the live list.
function visibleFrames() {
  if (state.selectedFrame && state.frozenFrames) return state.frozenFrames.filter(matchesFilters);
  return liveFrames();
}
// One time-ordered list, newest first: verified, partial and candidate rows interleaved.
function liveFrames() {
  const byId = new Map(state.frames.map(f => [frameId(f), f]));
  for (const f of state.verifiedFrames) byId.set(frameId(f), f);
  return [...byId.values()].sort((a, b) => (b.capture_id - a.capture_id) || (b.start_sample - a.start_sample)).filter(matchesFilters);
}
function selectFrame(f) {
  if (!state.selectedFrame || !state.frozenFrames) state.frozenFrames = liveFrames();
  state.selectedFrame = f; selectPoints('frame'); renderFrames();
}
function resumeLive() { state.selectedFrame = null; state.frozenFrames = null; selectPoints('corrected'); renderFrames(); }
function frameCalibrationOffset(frame) {
  if (!frame?.calibration_settings || !Number.isFinite(frame.frequency_hz)) return null;
  const offsets = calibrationOffsets(frame.calibration_settings);
  return offsets.length ? calibrationOffset(frame.frequency_hz, offsets) : null;
}
// SNR and noise floor are measured in the occupied OFDM band (+-8.125 MHz); the noise
// floor comes from the quiet part of the same capture (native/snr.hpp). The L-LTF SNR
// comes from the decoder: noise is the difference of the two identical LTF symbols.
function snrText(value) { return Number.isFinite(value) ? `${value.toFixed(1)} dB` : 'Unavailable'; }
function framePowerText(value, offset) {
  if (!Number.isFinite(value)) return 'Unavailable';
  return `${value.toFixed(2)} dBFS${Number.isFinite(offset) ? ` (${(value + offset).toFixed(1)} dBm)` : ''}`;
}
// mpdu_hex excludes the four FCS bytes in both decoder backends.
function frameSize(f) {
  return f?.fcs_ok && typeof f.mpdu_hex === 'string' && /^(?:[0-9a-f]{2})+$/i.test(f.mpdu_hex) ? f.mpdu_hex.length / 2 + 4 : null;
}
function decodeWinject(f) {
  const hex = f?.mpdu_hex;
  if (!f?.fcs_ok || typeof hex !== 'string' || !/^(?:[0-9a-f]{2})+$/i.test(hex)) return null;
  const bytes = hex.match(/../g).map(x => parseInt(x, 16));
  // Mpdu::rescan accepts plain data frames with no flags and exact slot layout.
  if (bytes.length < 24 || bytes[0] !== 0x08 || bytes[1] !== 0 ||
      bytes.slice(16, 20).join(',') !== '202,254,186,190') return null;
  const packed = bytes.slice(4, 16);
  const sizes = Array.from({length: 5}, (_, i) => {
    let value = 0;
    for (let bit = 0; bit < 11; bit++) {
      const n = 1 + i * 11 + bit;
      value |= ((packed[Math.floor(n / 8)] >> (n % 8)) & 1) << bit;
    }
    return value;
  });
  if (sizes.reduce((a, b) => a + b, 0) !== bytes.length - 24) return null;
  let offset = 24;
  const slots = sizes.map((size, index) => {
    const slot = {index, size, offset, bus: size ? bytes[offset] : null};
    offset += size;
    return slot;
  });
  return {domain: (bytes[20] << 8) | bytes[21], slots};
}
function renderMpdu(f, winject) {
  const el = $('#frame-mpdu'), hex = f?.mpdu_hex || f?.partial_mpdu_hex;
  el.replaceChildren();
  if (!hex) { el.textContent = 'Not decoded'; return; }
  if (!winject) { el.textContent = hex; return; }
  const add = (start, end, className, title) => {
    if (end <= start) return;
    const span = document.createElement('span');
    span.className = className; span.title = title;
    span.textContent = hex.slice(start * 2, end * 2); el.appendChild(span);
  };
  add(0, 24, 'mpdu-header', '802.11 / winject header');
  for (const slot of winject.slots) {
    if (!slot.size) continue;
    add(slot.offset, slot.offset + 1, `mpdu-bus slot-${slot.index}`, `Slot ${slot.index} bus: 0x${slot.bus.toString(16).padStart(2, '0')}`);
    add(slot.offset + 1, slot.offset + slot.size, `mpdu-pdu slot-${slot.index}`, `Slot ${slot.index} PDU`);
  }
}
// Partial frames run past the end of the capture: the host reports the SIGNAL length, the
// expected airtime and how much of it the capture holds. They can never be FCS verified.
function partialPercent(f) {
  return f?.partial && f.frame_duration_expected_us > 0 && Number.isFinite(f.captured_us) ? Math.round(100 * f.captured_us / f.frame_duration_expected_us) : null;
}
function resultText(f) {
  if (f.partial) { const pct = partialPercent(f); return pct === null ? 'Partial' : `Partial · ${pct} %`; }
  return f.status || 'Candidate';
}
function frameSizeText(f) {
  if (f.partial && Number.isFinite(f.signal_length_bytes)) return `~${f.captured_bytes_estimate ?? 0} of ${f.signal_length_bytes} B`;
  return frameSize(f) === null ? 'Unavailable' : `${frameSize(f)} B`;
}
function capturedText(f) {
  if (!f) return '—';
  if (f.partial) {
    const bytes = Number.isFinite(f.signal_length_bytes) ? `~${f.captured_bytes_estimate ?? 0} of ${f.signal_length_bytes} B` : null;
    const time = Number.isFinite(f.captured_us) && Number.isFinite(f.frame_duration_expected_us) ? `${f.captured_us.toFixed(0)} of ${f.frame_duration_expected_us.toFixed(0)} µs` : null;
    return ['Partial', bytes, time].filter(Boolean).join(' · ');
  }
  if (f.partial === false || f.fcs_ok) return 'Complete';
  return 'Unavailable';
}
function frameRowValues(f) {
  const address = frameAddresses(f);
  return [`${f.capture_id}.${f.start_sample}`, readable(f.phy), modulationRate(f),
    evmText(f.evm_percent), framePowerText(f.frame_mean_dbfs, frameCalibrationOffset(f)),
    snrText(f.snr_db), snrText(f.ltf_snr_db), framePowerText(f.noise_floor_dbfs, frameCalibrationOffset(f)),
    f.frame_clipped === true ? 'Yes' : f.frame_clipped === false ? 'No' : 'Unavailable',
    readable(address.addr3), resultText(f), frameSizeText(f)];
}
function renderFrames() {
  const list = visibleFrames();
  const allCount = state.frames.length + state.verifiedFrames.length;
  const held = !!(state.selectedFrame && state.frozenFrames);
  $('#frame-count').textContent = held ? `View held · ${list.length} shown · capture continues` : `${state.verifiedFrames.length} FCS verified · ${list.length} shown`;
  $('#frames-resume').hidden = !held;
  const body = $('#frames-body'); body.replaceChildren();
  for (const f of list) {
    const tr = document.createElement('tr');
    if (state.selectedFrame && frameId(state.selectedFrame) === frameId(f)) tr.classList.add('selected');
    if (f.partial) tr.classList.add('partial');
    for (const value of frameRowValues(f)) {
      const td = document.createElement('td'); td.textContent = String(value); tr.appendChild(td);
    }
    tr.frame = f;
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
  $('#frame-captured').textContent = capturedText(f);
  const unverified = f?.header_verified === false;
  for (const key of ['addr1', 'addr2', 'addr3']) $(`#frame-${key}`).textContent = f ? readable(a[key]) + (unverified && a[key] ? ' (unverified)' : '') : '—';
  $('#frame-evm').textContent = f ? evmText(f.evm_percent) : 'Unavailable';
  $('#frame-status').textContent = f ? [f.reason || f.status || 'Preamble candidate; frame not decoded.', f.fcs_ok ? f.evm_warning : f.training_warning].filter(Boolean).join(' ') : 'Decoded frame fields appear when available.';
  const winject = decodeWinject(f);
  renderMpdu(f, winject);
  $('#frame-winject').textContent = winject ? [`Domain: 0x${winject.domain.toString(16).padStart(4, '0')}`, `MPDU: ${frameSize(f)} B`, ...winject.slots.map(s => `#${s.index}: ${s.size} B${s.bus === null ? '' : ` · bus 0x${s.bus.toString(16).padStart(2, '0')}`}`)].join('\n') : 'Not a decoded winject frame';
  $('#frame-gain').textContent = Number.isFinite(f?.gain_index) ? `${f.gain_index} (${f.agc === true ? 'AGC' : f.agc === false ? 'manual' : 'mode unknown'})` : 'Unavailable';
  const frameOffset = frameCalibrationOffset(f);
  for (const [id, key] of [['peak', 'frame_peak_dbfs'], ['mean', 'frame_mean_dbfs'], ['noise', 'noise_floor_dbfs']]) $(`#frame-${id}`).textContent = framePowerText(f?.[key], frameOffset);
  $('#frame-snr').textContent = snrText(f?.snr_db);
  $('#frame-ltf-snr').textContent = snrText(f?.ltf_snr_db);
  const fields = ['frame_type', 'frame_subtype', 'to_ds', 'from_ds', 'protected', 'sequence', 'fragment', 'header_length', 'addr4'];
  $('#frame-decoded').textContent = f?.fcs_ok ? [frameSize(f) === null ? null : `frame size: ${frameSize(f)} B`, ...fields.filter(k => f[k] !== undefined && f[k] !== null).map(k => `${k.replaceAll('_', ' ')}: ${f[k]}`)].filter(Boolean).join('\n') || 'No additional fields' : 'Not decoded';
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
  const points = src?.raw_training_iq || [];
  $('#training-empty').hidden = points.length > 0;
  $('#training-empty').textContent = src ? 'Raw training IQ unavailable for this frame' : 'Waiting for frame samples';
  x.fillStyle = '#36e5d2aa';
  for (const p of points) if (Number.isFinite(p[0]) && Number.isFinite(p[1])) { x.beginPath(); x.arc(plot.cx+p[0]*plot.scale,plot.cy-p[1]*plot.scale,2,0,7); x.fill(); }
}
// Pilot-corrected data symbols exist for verified frames and for partial frames whose
// captured symbols were decoded (pilots are in every symbol; only the FCS is missing).
function hasDataSymbols(f) { return !!f && (f.fcs_ok === true || (f.partial === true && f.header_verified === false)); }
function renderConstellation() {
  const isFrame = state.points === 'frame', snapshot = state.points === 'snapshot';
  const corrected = state.points === 'corrected' || isFrame;
  const candidate = displayedCandidate();
  const liveFresh = state.status?.mode === 'live' && ws?.readyState === WebSocket.OPEN && performance.now() - state.liveAt < 3000;
  const src = corrected ? candidate : snapshot ? state.snapshot : liveFresh ? state.live : null;
  const pts = corrected ? (hasDataSymbols(src) ? src.constellation : []) : src?.points;
  const valid = Array.isArray(pts) ? pts.filter(p => Array.isArray(p) && Number.isFinite(p[0]) && Number.isFinite(p[1])) : [];
  $('#data-empty').hidden = valid.length > 0;
  $('#data-empty').textContent = src ? 'Decoded data symbols unavailable for this frame' : 'Waiting for decoded data symbols';
  const showPayload = corrected && hasDataSymbols(src) && valid.length > 0, partialPayload = showPayload && !src.fcs_ok;
  const trainingEvm = corrected && !showPayload && valid.length > 0 && Number.isFinite(src?.training_evm_percent);
  const {x, w, h} = canvas('#constellation-canvas'); x.clearRect(0, 0, w, h); const plot = constellationGrid(x, w, h);
  if (valid.length) {
    x.fillStyle = isFrame ? '#ffb86b99' : '#46a4ff99';
    for (const [i, q] of valid) { x.beginPath(); x.arc(plot.cx + i * plot.scale, plot.cy - q * plot.scale, 1.8, 0, 7); x.fill(); }
  }
  drawTraining(candidate);
  const raw = Array.isArray(candidate?.raw_data_iq) ? candidate.raw_data_iq.filter(p => Array.isArray(p) && Number.isFinite(p[0]) && Number.isFinite(p[1])) : [];
  const rawCanvas = canvas('#raw-data-canvas'); rawCanvas.x.clearRect(0, 0, rawCanvas.w, rawCanvas.h); const rawPlot = constellationGrid(rawCanvas.x, rawCanvas.w, rawCanvas.h);
  $('#raw-data-empty').hidden = raw.length > 0;
  $('#raw-data-empty').textContent = candidate ? 'Raw data IQ unavailable for this frame' : 'Waiting for frame samples';
  rawCanvas.x.fillStyle = '#46a4ff99';
  for (const [i, q] of raw) { rawCanvas.x.beginPath(); rawCanvas.x.arc(rawPlot.cx + i * rawPlot.scale, rawPlot.cy - q * rawPlot.scale, 1.8, 0, 7); rawCanvas.x.fill(); }
  $('#c-kind').textContent = isFrame ? 'SELECTED FRAME' : corrected ? (src ? `LIVE FRAME · CAPTURE ${src.capture_id}` : 'LIVE · WAITING FOR COMPLETE FRAME') : snapshot ? `SNAPSHOT #${src?.capture_id ?? '—'}` : 'LIVE RAW IQ';
  $('#c-title').textContent = partialPayload ? 'Decoded data-field constellation (partial)' : showPayload ? 'Decoded data-field constellation' : corrected ? 'Corrected constellation' : snapshot ? 'Frozen raw IQ' : 'Raw baseband IQ';
  const trainingName = src?.training_kind || (/ofdm/i.test(src?.phy || '') || /ofdm/i.test(src?.detection || '') ? 'L-LTF' : 'Training');
  const phyText = [src?.phy, modulationRate(src)].filter(x => x && x !== 'Unavailable').join(' · ');
  $('#c-description').textContent = partialPayload ? `Corrected data symbols captured before the end of the capture for a partial${phyText ? ` ${phyText}` : ''} frame; not FCS verified.` : showPayload ? `Corrected data symbols for an FCS-verified${phyText ? ` ${phyText}` : ''} frame.` : corrected ? (valid.length ? `Estimated equalized ${trainingName} training symbols for an unverified preamble candidate. These are not decoded payload symbols.` : 'No current corrected constellation is available. Waiting for a usable candidate.') : src ? 'Samples are shown before frame synchronization or equalization. They are not demodulated symbols.' : 'No current raw IQ burst is available.';
  $('#c-points').textContent = valid.length || '—';
  $('#c-freq').textContent = src?.frequency_hz ? mhz(src.frequency_hz) : '—';
  $('#c-rate').textContent = src?.sample_rate_hz ? `${src.sample_rate_hz / 1e6} MS/s` : '—';
  $('#c-evm-label').textContent = trainingEvm ? `${trainingName} EVM (estimate)` : 'Data-field EVM (estimate)';
  $('#c-evm').textContent = corrected ? evmText(trainingEvm ? src.training_evm_percent : src?.evm_percent) : 'Unavailable';
  $('#c-evm-note').textContent = showPayload ? (src?.evm_warning || src?.evm_scope || 'Data-field EVM estimate; not standards-calibrated.') : corrected && src?.training_warning ? src.training_warning : trainingEvm ? `${trainingName} EVM estimate; not payload EVM.` : 'EVM requires synchronized, equalized symbols and a known reference.';
}
function renderChannelResponse() {
  const candidate = displayedCandidate();
  renderFrameWaveform(candidate);
  const isOfdm = /ofdm/i.test(candidate?.phy || '') || /ofdm/i.test(candidate?.detection || '');
  const values = (isOfdm && Array.isArray(candidate?.channel_estimate) ? candidate.channel_estimate : []).filter(p => Array.isArray(p) && p.length >= 3 && p.every(Number.isFinite)).sort((a,b) => a[0] - b[0]);
  const {x, w, h} = canvas('#response-canvas'); x.clearRect(0, 0, w, h); grid(x, w, h, 13, 4);
  if (!values.length) { $('#response-meta').textContent = candidate && !isOfdm ? 'No OFDM subcarrier estimate for this PHY' : 'No current channel estimate'; return; }
  const magnitude = values.map(p => Math.hypot(p[1], p[2]));
  const peak = Math.max(...magnitude);
  if (!(peak > 0)) { $('#response-meta').textContent = 'No usable channel estimate'; return; }
  const db = magnitude.map(v => Math.max(-40, 20 * Math.log10(Math.max(v / peak, 1e-4))));
  const pad=16, X=k => pad + (k + 26) / 52 * (w - 2 * pad), Y=v => 8 + (-v / 40) * (h - 20);
  trace(x, values.map(p=>p[0]), db, X, Y, h, '#36e5d2');
  x.fillStyle = '#7f97a3'; x.font = '11px monospace'; x.fillText('0 dB', 5, 12); x.fillText('−40 dB', 5, h - 6);
  const time = Number.isFinite(candidate.timestamp_us) ? ` · device trigger ~${candidate.timestamp_us} µs (u32)` : '';
  $('#response-meta').textContent = `${state.points === 'frame' ? 'Selected' : 'Live'} candidate · capture ${candidate.capture_id} · sample ${candidate.start_sample}${time} · ${values.length} carriers`;
}
function renderFrameWaveform(frame) {
  const fft = state.framePlot === 'fft', values = fft ? frame?.frame_spectrum_db : frame?.frame_envelope_db;
  const {x,w,h} = canvas('#channel-canvas'); x.clearRect(0,0,w,h); grid(x,w,h);
  const offset = frameCalibrationOffset(frame);
  $('#channel-note').textContent = fft ? (Number.isFinite(offset) ? `Mean spectral power per FFT bin · dBm estimated using center-frequency calibration and capture-start gain.${frame?.agc ? ' AGC may change during the frame.' : ''}${frame?.frame_clipped ? ' Samples clipped.' : ''}` : 'Mean spectral power per FFT bin · dBFS. dBm requires matching calibration.') : Number.isFinite(offset) ? `Peak power per time bucket; dBm is estimated using capture-start gain.${frame?.agc ? ' AGC may change during the frame.' : ''}${frame?.frame_clipped ? ' Samples clipped.' : ''}` : 'Peak power per time bucket · dBFS. dBm requires matching calibration.';
  $('#plot-scale').textContent = fft ? `−${(frame?.sample_rate_hz || 0)/2e6} MHz                                +${(frame?.sample_rate_hz || 0)/2e6} MHz` : `0 µs                                ${frame?.frame_duration_us ?? '—'} µs`;
  if (!Array.isArray(values) || values.length < 2) { $('#channel-meta').textContent = 'Complete frame waveform unavailable'; return; }
  const min = Math.floor(Math.min(...values)/10)*10, max = Math.max(min+10, Math.ceil(Math.max(...values)/10)*10);
  trace(x, values.map((_,i)=>i), values, i=>i/(values.length-1)*w, v=>h-(v-min)/(max-min)*h, h, '#36e5d2'); spectrumAxisLabels(x,w,h,min,max,offset);
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

function beginSweep(sweep) {
  if (state.sweep.job === sweep.job) return;
  sweepWaterfall.rows = []; sweepWaterfall.key = null;
  $('#sweep-waterfall-space').style.height = '0px';
  $('#sweep-waterfall-scroll').scrollTop = 0;
  const sweepCanvas = $('#sweep-waterfall');
  sweepCanvas.getContext('2d').clearRect(0, 0, sweepCanvas.width, sweepCanvas.height);
  const end = sweep.end_mhz ?? sweep.stop_mhz;
  state.sweep = {job: sweep.job, pending: [], stepHz: sweep.step_mhz * 1e6,
    range: [sweep.start_mhz * 1e6, end * 1e6]};
  $('#start-sweep').textContent = 'Pause';
  $('#sweep-progress').textContent = `0 / ${sweep.steps}`;
  $('#sweep-progress-bar').value = 0;
  $('#scale-start').textContent = `${sweep.start_mhz} MHz`;
  $('#scale-stop').textContent = `${end} MHz`;
  renderSweepRadioSettings();
  renderSweep();
}

function collectSweepPeek(tile) {
  const sweep = state.sweep;
  if (!sweep.pending || tile.job !== sweep.job) return;
  sweep.radioSettings = tile.calibration_settings;
  renderSweepRadioSettings();
  if (tile.index === 0) sweep.pending = [];
  if (tile.index !== sweep.pending.length || !(tile.total > 0) || tile.index >= tile.total) return;
  $('#sweep-progress').textContent = `${tile.index + 1} / ${tile.total} · gain ${tile.gain_index}`;
  $('#sweep-progress-bar').value = (tile.index + 1) / tile.total * 100;
  sweep.pending.push(tile);
  const peeks = sweep.pending;
  const {power, binHz} = combineSweepPeeks(peeks, ...sweep.range, sweep.stepHz);
  if (peeks.length !== tile.total) {
    const previous = sweep.power;
    sweep.displayPower = power.map((value, i) => value > 0
      ? (previous?.length === power.length && previous[i] > 0 ? 0.2 * previous[i] + 0.8 * value : value)
      : (previous?.length === power.length ? previous[i] : 0));
    sweep.binHz = binHz;
    const settings = peeks[0].calibration_settings;
    sweep.settings = settings && peeks.every(t => sameCalibrationSettings(settings, t.calibration_settings)) ? settings : null;
    renderSweep();
    return;
  }
  delete sweep.displayPower;
  sweep.power = averageSweepPower(sweep.power, power);
  addWaterfallCapture({spectrum: {start_hz: sweep.range[0], bin_hz: binHz, db: Array.from(sweep.power, v => v > 0 ? 10 * Math.log10(v) : NaN)}}, sweepWaterfall);
  sweep.binHz = binHz;
  const settings = peeks[0].calibration_settings;
  sweep.settings = settings && peeks.every(t => sameCalibrationSettings(settings, t.calibration_settings)) ? settings : null;
  sweep.pending = [];
  renderSweep();
}

function averageSweepPower(previous, current) {
  if (!previous || previous.length !== current.length) return current;
  return current.map((value, i) => previous[i] > 0 && value > 0 ? 0.2 * previous[i] + 0.8 * value : value);
}

function sweepTaper(distance, width) {
  const x = Math.max(0, Math.min(1, distance / width));
  return x * x * (3 - 2 * x);
}

function combineSweepPeeks(tiles, lo, hi, stepHz) {
  const binHz = tiles[0].bin_hz;
  const power = new Float64Array(Math.floor((hi - lo) / binHz) + 1);
  const weights = new Float64Array(power.length);
  for (const tile of tiles) {
    const end = tile.start_hz + (tile.bins.length - 1) * tile.bin_hz;
    const edgeWidth = Math.max(binHz, (end - tile.start_hz - stepHz) / 2);
    const first = Math.max(0, Math.ceil((tile.start_hz - lo) / binHz));
    const last = Math.min(power.length - 1, Math.floor((end - lo) / binHz));
    const samples = tile.bins.map(db => 10 ** (db / 10));
    for (let i = first; i <= last; i++) {
      const f = lo + i * binHz;
      // Fade overlapping edges and the region around the receiver's DC spur.
      const edge = Math.min(f - tile.start_hz, end - f) + tile.bin_hz / 2;
      const fromDc = Math.abs(f - tile.frequency_hz) - (tile.dc_guard_hz || 0);
      const dcWeight = tile.dc_guard_hz ? sweepTaper(fromDc, Math.max(binHz, 500_000)) : 1;
      const weight = sweepTaper(edge, edgeWidth) * dcWeight;
      const source = Math.max(0, Math.min(samples.length - 1, (f - tile.start_hz) / tile.bin_hz));
      const left = Math.floor(source), right = Math.min(left + 1, samples.length - 1);
      const value = samples[left] + (source - left) * (samples[right] - samples[left]);
      power[i] += value * weight;
      weights[i] += weight;
    }
  }
  for (let i = 0; i < power.length; i++) {
    if (weights[i] > 0) power[i] /= weights[i];
  }
  return {power, binHz};
}

let sweepCursor = null;
function updateSweepTooltip() {
  const tooltip = $('#sweep-tooltip'), {binHz, range, settings} = state.sweep;
  const power = state.sweep.displayPower || state.sweep.power;
  if (!sweepCursor || !power || !range || !(binHz > 0)) { tooltip.hidden = true; return; }
  const rect = $('#sweep-canvas').getBoundingClientRect();
  const fraction = (sweepCursor.x - rect.left) / rect.width;
  if (!(fraction >= 0 && fraction <= 1) || sweepCursor.y < rect.top || sweepCursor.y > rect.bottom) { tooltip.hidden = true; return; }
  const hz = range[0] + fraction * (range[1] - range[0]);
  const index = Math.round((hz - range[0]) / binHz);
  const value = power[index];
  if (!(value > 0) || !Number.isFinite(value)) { tooltip.hidden = true; return; }
  const db = 10 * Math.log10(value), offsets = settings ? calibrationOffsets(settings) : [];
  tooltip.textContent = `${(hz / 1e6).toFixed(3)} MHz\n${db.toFixed(1)} dBFS${offsets.length ? ` (${(db + calibrationOffset(hz, offsets)).toFixed(1)} dBm)` : ''}`;
  tooltip.hidden = false;
  tooltip.style.left = `${Math.max(0, Math.min(sweepCursor.x + 12, window.innerWidth - tooltip.offsetWidth - 4))}px`;
  tooltip.style.top = `${Math.max(0, Math.min(sweepCursor.y + 12, window.innerHeight - tooltip.offsetHeight - 4))}px`;
}
$('#sweep-canvas').addEventListener('mousemove', e => { sweepCursor = {x: e.clientX, y: e.clientY}; updateSweepTooltip(); });
$('#sweep-canvas').addEventListener('mouseleave', () => { sweepCursor = null; updateSweepTooltip(); });

function renderSweep() {
  updateSweepTooltip();
  const {x, w, h} = canvas('#sweep-canvas'); if (!w || !h) return;
  x.clearRect(0, 0, w, h); grid(x, w, h, 10, 4);
  const {binHz, range, settings} = state.sweep;
  const power = state.sweep.displayPower || state.sweep.power;
  if (!power) return;
  const [lo, hi] = range;
  const db = power.map(value => 10 * Math.log10(value));
  let low = Infinity, high = -Infinity;
  for (const value of db) {
    if (!Number.isFinite(value)) continue;
    low = Math.min(low, value);
    high = Math.max(high, value);
  }
  if (!Number.isFinite(low)) return;
  const min = Math.floor(low / 10) * 10 - 5, max = Math.ceil(high / 10) * 10 + 5;
  const X = f => (f - lo) / (hi - lo) * w, Y = v => h - (v - min) / (max - min) * h;
  x.strokeStyle = '#36e5d2';
  x.lineWidth = 1.3;
  x.beginPath();
  let connected = false;
  for (let i = 0; i < power.length; i++) {
    if (!Number.isFinite(db[i])) {
      connected = false;
      continue;
    }
    const px = X(lo + i * binHz), py = Y(db[i]);
    if (connected) x.lineTo(px, py);
    else x.moveTo(px, py);
    connected = true;
  }
  x.stroke();
  const offsets = settings ? calibrationOffsets(settings) : [];
  const offset = offsets.length ? calibrationOffset((lo + hi) / 2, offsets) : null;
  spectrumAxisLabels(x, w, h, min, max, offset);
  spectrumFrequencyLabels(x, w, h, lo, hi);
  $('#scale-start').textContent = mhz(lo); $('#scale-stop').textContent = mhz(hi);
  $('#sweep-axis-note').textContent = offsets.length ? 'dBm scale uses center-frequency calibration.' : `dBm unavailable: ${calibrationMissingReason(settings, false)}`;
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
$('#filter-complete').addEventListener('change', () => { state.completeOnly = $('#filter-complete').checked; filtersChanged(); savePreferences(); });
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
// The table re-renders several times a second while live, so a row can be replaced
// between mouse press and release and the click never fires. Select on the press
// for a mouse; touch keeps the click so starting a scroll does not select a frame.
$('#frames-body').addEventListener('pointerdown', e => {
  if (e.pointerType !== 'mouse' || e.button !== 0) return;
  const tr = e.target?.closest?.('tr');
  if (!tr?.frame) return;
  $('#frames-scroll').focus(); selectFrame(tr.frame);
});
$('#return-live').onclick = resumeLive;
$('#frames-resume').onclick = resumeLive;
$('#run').onclick = () => send({op: 'live', on: state.status?.mode !== 'live'});
$('#snapshot').onclick = () => send({op: 'snapshot'});
for (const key of ['addr1', 'addr2', 'addr3']) {
  $(`#filter-${key}`).addEventListener('input', () => {
    const values = Object.fromEntries(['addr1', 'addr2', 'addr3'].map(k => [k, normalizeMac($(`#filter-${k}`).value)]));
    if (Object.values(values).some(v => v === null)) { savePreferences(); $('#filter-error').textContent = 'Use six hexadecimal octets, for example ca:fe:ba:be:00:01.'; return; }
    $('#filter-error').textContent = ''; state.filters = values; filtersChanged(); savePreferences();
  });
}
$('#clear-filters').onclick = () => { for (const key of ['addr1', 'addr2', 'addr3']) $(`#filter-${key}`).value = ''; state.filters = {addr1: '', addr2: '', addr3: ''}; state.fcsOnly = false; $('#filter-fcs').checked = false; state.completeOnly = false; $('#filter-complete').checked = false; $('#filter-error').textContent = ''; filtersChanged(); savePreferences(); };
const sweepRateHz = 40_000_000;
const sweepLpfMhz = 20;

function renderSweepRadioSettings() {
  const rate = state.sweep.radioSettings?.rate_hz ?? sweepRateHz;
  const lpf = state.sweep.radioSettings?.bw_mhz ?? sweepLpfMhz;
  $('#sweep-sample-rate').textContent = `${rate / 1e6} MS/s`;
  $('#sweep-lpf').textContent = lpf ? `${lpf} MHz` : 'Auto';
}

function applySweepSettings() {
  const fields = sweepNumberFields.map(name => $(`#sweep-${name}`));
  if (fields.some(el => !el.value.trim() || (el.validity && !el.validity.valid))) { toast('Check sweep capture settings'); return; }
  const [start, end, bw, overlap, time, gain] = fields.map(el => Number(el.value));
  // Adjacent peeks must cover each other's tuning centers away from the edges.
  const step = Math.min(bw - overlap, Math.max(1, Math.floor(bw * 0.4)));
  const stop = start + Math.ceil((end - start) / step) * step;
  if (![start,end,bw,overlap,time,gain].every(Number.isInteger) || start < 100 || end > 6000 || end <= start || bw < 2 || bw > sweepLpfMhz || overlap < 0 || step < 1 || time < 1 || time > 1000 || gain < 0 || gain > Number($('#sweep-gain').max || 81) || stop > 6000) { toast('Check frequency range, bandwidth, overlap and peek time'); return; }
  savePreferences();
  send({op:'sweep', start_mhz:start, end_mhz:end, stop_mhz:stop, step_mhz:step, rate_hz:sweepRateHz, lpf_mhz:sweepLpfMhz, peek_bw_mhz:bw,
    gain, fft:state.status?.config?.nfft || 1024, samples:state.status?.config?.n || 16380, bits:Number($('#sweep-bits').value), dwell_ms:time, detector:$('#detector').value, repeat:$('#sweep-repeat').checked});
}

$('#start-sweep').onclick = () => {
  if (state.sweep.pending) send({op: 'stop_sweep'});
  else applySweepSettings();
};
for (const id of [...sweepValueFields, 'sweep-repeat']) {
  $(`#${id}`).addEventListener('change', () => {
    if (state.sweep.pending) applySweepSettings();
    else savePreferences();
  });
}

function renderAll() { renderLive(); renderWaterfall(); renderWaterfall(sweepWaterfall); renderCalibration(); renderConstellation(); renderChannelResponse(); renderFrames(); if (state.live) renderPower(state.live); renderSweep(); }
addEventListener('resize', renderAll);
restorePreferences();
renderSweepRadioSettings();
loadCalibration();
connect();

$('#radio-connect').addEventListener('submit', e => { e.preventDefault(); if (ws?.readyState !== 1) { toast('Host connection unavailable'); return; } send({op:'radio.connect', address:$('#radio-address').value.trim()}); toast('Connecting to analyzer'); });

$('#capture-toggle').addEventListener('click', () => { const settings = $('#capture-settings'); const expanded = settings.classList.toggle('mobile-expanded'); $('#capture-toggle').setAttribute('aria-expanded', String(expanded)); $('#capture-toggle').textContent = expanded ? 'Hide' : 'Show'; });

$('#sweep-settings-toggle').addEventListener('click', () => { const settings = $('#sweep-settings'); const expanded = settings.classList.toggle('mobile-expanded'); $('#sweep-settings-toggle').setAttribute('aria-expanded', String(expanded)); $('#sweep-settings-toggle').textContent = expanded ? 'Hide' : 'Show'; });
