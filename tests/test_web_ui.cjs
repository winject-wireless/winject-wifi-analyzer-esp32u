const fs=require('fs'),vm=require('vm'),assert=require('assert');
const handlers={}; const store={}; const context2d={clearRect(){},setTransform(){},beginPath(){},moveTo(){},lineTo(){},stroke(){},arc(){},fill(){},closePath(){},fillText(){},createLinearGradient(){return {addColorStop(){}}}};
function elem(id){if(!store[id])store[id]={id,textContent:'',value:'',hidden:false,dataset:{},style:{},offsetWidth:100,offsetHeight:40,classList:{add(){},remove(){},toggle(){}},getBoundingClientRect(){return {left:0,top:0,bottom:240,width:400,height:240}},getContext(){return context2d},replaceChildren(){},appendChild(){},focus(){},children:[],addEventListener(type,fn){handlers[id+':'+type]=fn}};return store[id]}
class WS {static OPEN=1;constructor(){this.readyState=1}send(){}}
let now=10000;
const ctx={console,document:{querySelector:elem,querySelectorAll(){return []},createElement:elem},window:{innerWidth:400,innerHeight:240},WebSocket:WS,location:{protocol:'http:',host:'x'},devicePixelRatio:1,performance:{now:()=>now},setTimeout(){},clearTimeout(){},addEventListener(){}};
vm.createContext(ctx);vm.runInContext(fs.readFileSync('web/app.js','utf8'),ctx);
function go(s){return vm.runInContext(s,ctx)}
go("state.status={mode:'live'}; state.live={points:[[1,1]],frequency_hz:2412000000,sample_rate_hz:40000000}; state.liveAt=performance.now(); renderConstellation()");
assert.equal(elem('#c-title').textContent,'Corrected constellation');assert.equal(elem('#c-points').textContent,'—');
go("onFrames({history:true,frames:[{capture_id:1,start_sample:7,detection:'ofdm_preamble_candidate',constellation:[[1,0]],training_evm_percent:9,channel_estimate:[[1,1,0]]}]});renderConstellation()");assert.equal(elem('#c-points').textContent,'—','history must not be shown as live');
go("onFrames({frames:[{capture_id:2,start_sample:8,detection:'ofdm_preamble_candidate',constellation:[[1,0]],training_evm_percent:9,channel_estimate:[[1,1,0]]}]})");assert.equal(elem('#c-points').textContent,'—');assert.equal(elem('#response-meta').textContent,'No current channel estimate');
go("onFrames({frames:[{capture_id:3,start_sample:9,fcs_ok:true,phy:'OFDM',constellation:[[1,0]],training_constellation:[[1,0]],channel_estimate:[[1,1,0]]}]})");
assert.equal(elem('#c-points').textContent,1);assert(elem('#response-meta').textContent.includes('capture 3'));
now+=4000;go("onFrames({frames:[{capture_id:4,start_sample:10,detection:'ofdm_preamble_candidate',channel_estimate:[[1,2,0]]}]});renderConstellation();renderChannelResponse()");
assert.equal(elem('#c-points').textContent,1);assert(elem('#response-meta').textContent.includes('capture 3'));assert.equal(go('state.frames.length'),4);
go("state.selectedFrame=state.frames[1];selectPoints('frame');renderChannelResponse()");assert.equal(elem('#c-points').textContent,'—');assert(elem('#response-meta').textContent.includes('Selected'));
go("handle({event:'frames.clear'})");assert.equal(go('state.frames.length'),0);assert.equal(go('state.points'),'corrected');
console.log('mocked UI: corrected default, history, freshness, selected frame and clear passed');
go("applyStatus({mode:'live', radio:{}, radio_ip:'radio', captures_per_s:5, lost:0, errors:0, detector_supported:true, detector_processed:7, detector_dropped:1, decoder_backend:'native-cpp', decoder_formats:['legacy OFDM','DSSS'], decoder_rates_mbps:[1,2,6], decoder_attempted:4, decoder_decoded:2, decoder_dropped:0})");
assert(elem('#detector-status').textContent.includes('native-cpp'));
assert(elem('#detector-status').textContent.includes('legacy OFDM, DSSS'));
assert(elem('#detector-status').textContent.includes('1, 2, 6 Mb/s'));
go("state.filters={addr1:'',addr2:'',addr3:'ca:fe:ba:be:00:01'}");
assert.equal(go("matchesFilters({fcs_ok:false,addr3:'ca:fe:ba:be:00:01'})"),false,'unverified addresses must not match filters');
assert.equal(go("matchesFilters({fcs_ok:true,addr3:'ca:fe:ba:be:00:01'})"),true,'verified addresses must match exact filters');
go("state.selectedFrame={capture_id:5,start_sample:30,fcs_ok:true,phy:'802.11b',modulation:'DQPSK',rate_mbps:2,addr3:'ca:fe:ba:be:00:01',constellation:[[1,0]],evm_percent:5};selectPoints('frame');renderFrames()");
assert.equal(elem('#frame-phy').textContent,'802.11b');assert.equal(elem('#frame-modulation').textContent,'DQPSK · 2 Mb/s');
assert(elem('#c-description').textContent.includes('802.11b · DQPSK · 2 Mb/s'));assert(!elem('#c-description').textContent.includes('L-LTF'));
console.log('mocked UI: dynamic capabilities, verified address filters and DSSS labels passed');

go("renderChannelResponse()");assert.equal(elem('#response-meta').textContent,'No OFDM subcarrier estimate for this PHY');

go("state.fcsOnly=true"); assert.equal(go("matchesFilters({fcs_ok:false})"),false);
go("state.filters={addr1:'',addr2:'',addr3:''}; state.selectedFrame={capture_id:6,start_sample:0,fcs_ok:true,frame_peak_dbfs:-3,frame_mean_dbfs:-12,frame_type:2,frame_spectrum_db:[-30,-10],frame_duration_us:20};state.points='frame';state.framePlot='fft';renderFrames();renderChannelResponse()");
assert.equal(elem('#frame-peak').textContent,'-3.00 dBFS');assert(elem('#frame-decoded').textContent.includes('frame type: 2'));assert(elem('#channel-meta').textContent.includes('20 µs'));

go("state.points='corrected';state.selectedFrame=null;state.fcsOnly=false;state.filters={addr1:'',addr2:'',addr3:'ca:fe:ba:be:00:01'};filtersChanged()");
assert.equal(go('state.liveCandidate'),null);
go("onFrames({frames:[{capture_id:10,start_sample:0,fcs_ok:true,addr3:'ca:fe:ba:be:00:02',constellation:[[1,0]]}]})");
assert.equal(go('state.liveCandidate'),null);assert(go("state.frames.some(f=>f.capture_id===10)"));
go("onFrames({frames:[{capture_id:11,start_sample:0,fcs_ok:true,addr3:'ca:fe:ba:be:00:01',constellation:[[1,0]]}]})");
assert.equal(go('state.liveCandidate.capture_id'),11);
go("onFrames({frames:[{capture_id:12,start_sample:0,fcs_ok:true,addr3:'ca:fe:ba:be:00:02'}]})");
assert.equal(go('state.liveCandidate.capture_id'),11);

// A reload restores browser preferences without sending capture configuration.
const navs = [{dataset:{view:'constellation'}}, {dataset:{view:'live'}}];
const plots = [{dataset:{framePlot:'channel'}}, {dataset:{framePlot:'fft'}}];
const detectors = [{dataset:{detector:'average'}}, {dataset:{detector:'peak'}}];
for (const el of [...navs, ...plots, ...detectors]) {
  el.active = false;
  el.classList = {add(){el.active=true}, remove(){el.active=false}, toggle(_name, value){el.active=value}};
}
elem('.capture-column').prepend = () => {};
ctx.document.querySelectorAll = selector => ({'.nav':navs, '.nav,.view':navs, '[data-frame-plot]':plots, '[data-detector]':detectors}[selector] || []);
ctx.document.querySelector = selector => selector === '.nav.active' ? navs.find(n => n.active) : selector === '[data-detector].active' ? detectors.find(n => n.active) : elem(selector);
let savedPreferences = JSON.stringify({view:'live', detector:'peak', framePlot:'fft', fcsOnly:true, filters:{addr1:'aa:bb:cc:dd:ee:ff',addr2:'',addr3:''}, filterInputs:{addr1:'AA-BB-CC-DD-EE-FF',addr2:'partial',addr3:''}});
ctx.localStorage = {getItem(){return savedPreferences}, setItem(_key,value){savedPreferences=value}};
go('restorePreferences()');
assert(navs[1].active && detectors[1].active && plots[1].active);
assert.equal(go('state.framePlot'), 'fft');
assert.equal(go('state.filters.addr1'), 'aa:bb:cc:dd:ee:ff');
assert.equal(elem('#filter-addr2').value, 'partial');
assert(elem('#filter-error').textContent.includes('six hexadecimal octets'));
assert.equal(elem('#filter-fcs').checked, true);
go('savePreferences()');
assert.equal(JSON.parse(savedPreferences).view, 'live');
assert.equal(JSON.parse(savedPreferences).filterInputs.addr2, 'partial');
ctx.localStorage = {getItem(){throw Error('denied')}, setItem(){throw Error('denied')}};
assert.doesNotThrow(() => go('restorePreferences(); savePreferences()'));
console.log('mocked UI: view and filters survive reload, including unavailable storage');

// Calibration uses interpolated offsets only for the capture settings measured.
go(`state.calibration={version:1,settings:{gain:40,rate_hz:40000000,nfft:2,samples:128,detector:'average',bw_mhz:0,bits:8},points:[{hz:100,measured_dbfs:-50,actual_dbm:-30},{hz:200,measured_dbfs:-60,actual_dbm:-30}]}; state.status={config:{gain:40,bw_mhz:0,detector:'average'}}; state.live={samples:128,agc:false,gain_index:40,sample_rate_hz:40000000,bits:8,spectrum:{db:[-50,-60]}}`);
assert.equal(go('calibrationOffset(150)'),25);
assert.equal(go('calibrationOffset(50)'),20);
assert.equal(go('calibrationOffset(250)'),30);
assert.equal(go('calibrationMatches(state.live)'),true);
assert.equal(go('calibrationMatches({...state.live,agc:true})'),true);
assert.equal(go('calibrationMatches({...state.live,gain_index:41})'),false);
assert.equal(go('validCalibration(state.calibration)'),true);
assert.equal(go('validCalibration({...state.calibration,points:[state.calibration.points[0],state.calibration.points[0]]})'),true);
go("state.calibration.points.push({hz:100,measured_dbfs:-50,actual_dbm:-20,settings:state.calibration.settings}); state.calibration.points.push({hz:100,measured_dbfs:-50,actual_dbm:0,settings:{...state.calibration.settings,gain:41}})");
assert.equal(go('matchingCalibrationPoints(state.live).length'),3);
assert.equal(go('calibrationOffset(100)'),25);
console.log('mocked UI: calibration interpolation, limits and capture settings passed');

// Arrow keys navigate only the visible, filtered frame list.
go("state.filters={addr1:'',addr2:'',addr3:''};state.fcsOnly=false;state.verifiedFrames=[];state.frames=[{capture_id:20,start_sample:0,agc:true,gain_index:37},{capture_id:21,start_sample:0,agc:false,gain_index:40}];state.selectedFrame=null;renderFrames()");
let prevented=false; const key=k=>handlers['#frames-scroll:keydown']({key:k,preventDefault(){prevented=true}});
key('ArrowDown');assert(prevented);assert.equal(go('state.selectedFrame.capture_id'),21);assert.equal(elem('#frame-gain').textContent,'40 (manual)');
key('ArrowDown');assert.equal(go('state.selectedFrame.capture_id'),20);assert.equal(elem('#frame-gain').textContent,'37 (AGC)');
key('ArrowUp');assert.equal(go('state.selectedFrame.capture_id'),21);
assert(fs.readFileSync('web/index.html','utf8').includes('id="frame-gain"'));
console.log('mocked UI: frame gain and arrow navigation passed');

// Selecting a frame holds the table while capture continues; Resume live releases it.
go("state.selectedFrame=null;state.frozenFrames=null;state.frames=[{capture_id:30,start_sample:0},{capture_id:31,start_sample:0}];renderFrames()");
assert.equal(go('visibleFrames().length'),2);assert.equal(elem('#frames-resume').hidden,true);
go("selectFrame(state.frames[0])");
go("onFrames({frames:[{capture_id:32,start_sample:0,fcs_ok:true},{capture_id:33,start_sample:0}]})");
assert.deepEqual(go('visibleFrames().map(f=>f.capture_id)'),[31,30]);assert.equal(go('state.selectedFrame.capture_id'),30);
assert.equal(elem('#frames-resume').hidden,false);assert(elem('#frame-count').textContent.startsWith('View held'));
key('ArrowUp');assert.equal(go('state.selectedFrame.capture_id'),31);assert.deepEqual(go('visibleFrames().map(f=>f.capture_id)'),[31,30]);
elem('#frames-resume').onclick();
assert.equal(go('state.selectedFrame'),null);assert.deepEqual(go('visibleFrames().map(f=>f.capture_id)'),[32,33,31,30]);assert.equal(elem('#frames-resume').hidden,true);
go("selectFrame(state.frames[1])");elem('#return-live').onclick();assert.equal(go('state.frozenFrames'),null);
console.log('mocked UI: selected frame holds the table, Resume live releases it');

// Manually select the reference tone and reject frequencies near DC.
go("state.status.mode='live';state.live={spectrum:{start_hz:2400000000,bin_hz:100000,db:Array(32).fill(-90)}};state.live.spectrum.db[16]=0;state.live.spectrum.db[17]=-1;state.live.spectrum.db[20]=-10;state.live.spectrum.db[23]=-20");
elem('#calibration-auto').onclick();assert.equal(elem('#calibration-auto-frequency').value,'');
elem('#calibration-use-peak').onclick();assert.equal(elem('#calibration-auto-frequency').value,'2402.3');
elem('#calibration-spectrum').onclick({clientX:230,currentTarget:{getBoundingClientRect(){return {left:0,width:310}}}});
assert.equal(elem('#calibration-auto-frequency').value,'2402.3');assert(elem('#calibration-auto-tone').textContent.includes('Selected'));
elem('#calibration-auto-power').value='-60';elem('#calibration-auto-frequency').value='2401.6';
elem('#calibration-auto-form').onsubmit({preventDefault(){}});assert(elem('#calibration-auto-tone').textContent.includes('away from the center'));
console.log('mocked UI: manual tone selection and frequency validation passed');

// Transient peaks expand the spectrum axis but do not make it bounce on each update.
go("delete spectrumAxes.test; state.live={frequency_hz:2450000000,sample_rate_hz:40000000,gain_index:0,agc:false}");
assert.equal(go("spectrumScale('test',[-75,-56],'dBFS',state.live).max"),-45);
assert.equal(go("spectrumScale('test',[-75,-46],'dBFS',state.live).max"),-45);
assert.equal(go("spectrumScale('test',[-75,-41],'dBFS',state.live).max"),-35);
now+=1000;assert.equal(go("spectrumScale('test',[-75,-56],'dBFS',state.live).max"),-35);
now+=10000;assert.equal(go("spectrumScale('test',[-75,-56],'dBFS',state.live).max"),-45);
assert.equal(go("spectrumScale('test',[-75,-56],'dBm',state.live).max"),-45);
console.log('mocked UI: spectrum axes expand promptly and hold through transient peaks');

// Both scales share positions, with dBm referenced to the center calibration.
const axisLabels=[];context2d.fillText=(label,x,y)=>axisLabels.push({label,x,y});
go("spectrumAxisLabels(document.querySelector('#live-spectrum').getContext('2d'),400,240,-80,-40,20)");
assert.equal(axisLabels.length,10);assert.equal(axisLabels[0].label,'-40 dBFS');assert.equal(axisLabels[5].label,'-20 dBm');assert.equal(axisLabels[0].y,axisLabels[5].y);
axisLabels.length=0;go("spectrumAxisLabels(document.querySelector('#live-spectrum').getContext('2d'),400,240,-80,-40,null)");assert.equal(axisLabels.length,5);
console.log('mocked UI: aligned dBFS and calibrated dBm axis labels');

axisLabels.length=0;
go("spectrumFrequencyLabels(document.querySelector('#live-spectrum').getContext('2d'),1000,240,2430000000,2470000000)");
assert.equal(axisLabels.length,9);assert.equal(axisLabels[0].label,'2434.00');assert.equal(axisLabels[0].x,100);assert.equal(axisLabels[8].label,'2466.00');assert.equal(axisLabels[8].x,900);
go(`state.live={frequency_hz:2450000000,sample_rate_hz:40000000,samples:128,gain_index:40,agc:false,bits:8,spectrum:{db:Array(16).fill(-50),start_hz:2430000000,bin_hz:2500000}};state.status={config:{gain:40,bw_mhz:0,detector:'average'}};state.calibration={version:1,points:[{hz:2450000000,measured_dbfs:-50,actual_dbm:-30,settings:{gain:40,rate_hz:40000000,nfft:16,samples:128,detector:'average',bw_mhz:0,bits:8}}]}`);
axisLabels.length=0;go('renderCalibration()');assert(axisLabels.some(x=>x.label.endsWith('dBFS')));assert(axisLabels.some(x=>x.label.endsWith('dBm')));assert(elem('#calibration-axis-note').textContent.includes('center-frequency'));
console.log('mocked UI: frequency grid labels and calibration dual axes');

// Unmatched gain and capture settings must explain why dBm is unavailable.
assert(go("calibrationAxisNote({...state.live,gain_index:41,agc:true},false)").includes('Gain 41 is uncalibrated (AGC varies)'));
assert(go("calibrationAxisNote({...state.live,bits:10},false)").includes('IQ bit depth 10 → 8'));
assert(go("calibrationAxisNote({...state.live,agc:true},true)").includes('AGC may change during capture'));
go(`state.calibration.points[0].settings = {...state.calibration.points[0].settings,bits:10,bw_mhz:20};
renderCalibration()`);
assert.match(elem('#calibration-status').textContent, /Calibration inactive · gain 40.*IQ bit depth 8 → 10/);
go("state.status.config.bw_mhz=20; state.live.bits=10; renderCalibration()");
assert.equal(elem('#calibration-status').textContent,'Calibration active · gain 40');
go("state.calibration.points[0].settings.bw_mhz=0; state.calibration.points[0].settings.bits=8; state.status.config.bw_mhz=0; state.live.bits=8");
console.log('mocked UI: dBm availability explains missing gain and setting mismatches');

// Frame peak, mean and envelope use the selected frame's calibrated offset.
go(`state.selectedFrame={capture_id:55,start_sample:0,fcs_ok:true,frequency_hz:2450000000,agc:true,frame_clipped:true,frame_rssi_dbfs:-35,frame_peak_dbfs:0,frame_mean_dbfs:-10,frame_envelope_db:[-30,-20,0],frame_duration_us:10,calibration_settings:{gain:40,rate_hz:40000000,nfft:16,samples:128,detector:'average',bw_mhz:0,bits:8}};state.points='frame';state.framePlot='envelope';renderFrames()`);
assert.equal(elem('#frame-peak').textContent,'0.00 dBFS (20.0 dBm)');assert.equal(elem('#frame-mean').textContent,'-10.00 dBFS (10.0 dBm)');
axisLabels.length=0;go('renderChannelResponse()');assert(axisLabels.some(x=>x.label.endsWith('dBFS')));assert(axisLabels.some(x=>x.label.endsWith('dBm')));assert(elem('#channel-note').textContent.includes('Samples clipped'));
axisLabels.length=0;go("state.framePlot='fft';state.selectedFrame.frame_spectrum_db=[-50,-20];renderChannelResponse()");assert(axisLabels.some(x=>x.label.endsWith('dBm')));assert(elem('#channel-note').textContent.includes('center-frequency calibration'));
go("state.selectedFrame.calibration_settings.gain=41;renderChannelResponse()");assert(!axisLabels.slice(-5).some(x=>x.label.endsWith('dBm')));assert(elem('#channel-note').textContent.includes('matching calibration'));
go("state.selectedFrame.calibration_settings.gain=41;renderFrames();state.framePlot='envelope';renderChannelResponse()");assert.equal(elem('#frame-peak').textContent,'0.00 dBFS');assert(elem('#channel-note').textContent.includes('matching calibration'));
console.log('mocked UI: calibrated frame powers and envelope axis');
// Each row uses its own captured calibration and distinguishes unknown clipping.
go("state.selectedFrame.calibration_settings.gain=40;state.selectedFrame.evm_percent=4.25;state.selectedFrame.snr_db=21.34;state.selectedFrame.noise_floor_dbfs=-30;state.selectedFrame.ltf_snr_db=18.06");
let row=go('frameRowValues(state.selectedFrame)');
assert.equal(row[0],'55.0');assert.equal(row[3],'4.25 %');assert.equal(row[4],'-10.00 dBFS (10.0 dBm)');assert.equal(row[5],'21.3 dB');assert.equal(row[6],'18.1 dB');assert.equal(row[7],'-30.00 dBFS (-10.0 dBm)');assert.equal(row[8],'Yes');
go('renderFrames()');assert.equal(elem('#frame-snr').textContent,'21.3 dB');assert.equal(elem('#frame-noise').textContent,'-30.00 dBFS (-10.0 dBm)');assert.equal(elem('#frame-ltf-snr').textContent,'18.1 dB');
console.log('mocked UI: frame SNR and in-band noise floor');
go("state.selectedFrame.calibration_settings.gain=41;state.selectedFrame.frame_clipped=false");
row=go('frameRowValues(state.selectedFrame)');assert.equal(row[4],'-10.00 dBFS');assert.equal(row[7],'-30.00 dBFS');assert.equal(row[8],'No');
go("delete state.selectedFrame.frame_clipped; delete state.selectedFrame.frame_mean_dbfs; delete state.selectedFrame.evm_percent; state.selectedFrame.snr_db=null; delete state.selectedFrame.noise_floor_dbfs; delete state.selectedFrame.ltf_snr_db");
row=go('frameRowValues(state.selectedFrame)');assert.equal(row[3],'Unavailable');assert.equal(row[4],'Unavailable');assert.equal(row[5],'Unavailable');assert.equal(row[6],'Unavailable');assert.equal(row[7],'Unavailable');assert.equal(row[8],'Unavailable');
go('renderFrames()');assert.equal(elem('#frame-snr').textContent,'Unavailable');assert.equal(elem('#frame-noise').textContent,'Unavailable');assert.equal(elem('#frame-ltf-snr').textContent,'Unavailable');


// Sweep converts usable peek width and overlap to tuning steps and sends milliseconds.
for (const [id,value] of Object.entries({'#sweep-start':'2400','#sweep-stop':'2421','#sweep-bw':'12','#sweep-overlap':'2','#sweep-time':'50','#sweep-gain':'37','#sweep-bits':'10','#detector':'peak'})) elem(id).value=value;
go("state.status={mode:'live'}; ws={readyState:1,send:m=>globalThis.sentSweep=JSON.parse(m)}");
elem('#start-sweep').onclick();
assert.equal(go('sentSweep.bits'),10);assert.equal(go('sentSweep.gain'),37);assert.equal(go('sentSweep.step_mhz'),4);assert.equal(go('sentSweep.stop_mhz'),2424);assert.equal(go('sentSweep.dwell_ms'),50);assert.equal(go('sentSweep.detector'),'peak');
assert.equal(go('sentSweep.peek_bw_mhz'),12);
assert.equal(go('sentSweep.rate_hz'),40_000_000);
assert.equal(go('sentSweep.lpf_mhz'),20);
assert.equal(elem('#sweep-sample-rate').textContent,'40 MS/s');
assert.equal(elem('#sweep-lpf').textContent,'20 MHz');
elem('#sweep-bw').value='20';elem('#sweep-overlap').value='15';elem('#start-sweep').onclick();
assert.equal(go('sentSweep.peek_bw_mhz'),20);assert.equal(go('sentSweep.step_mhz'),5);
assert.equal(go('sentSweep.rate_hz'),40_000_000);
assert.equal(go('sentSweep.lpf_mhz'),20);
elem('#sweep-bw').value='21';go('sentSweep=null');elem('#start-sweep').onclick();assert.equal(go('sentSweep'),null);
const stitched = Array.from(go(`combineSweepPeeks([
  {start_hz: 0, bin_hz: 1, frequency_hz: 1, dc_guard_hz: 0.1, bins: [-70, -10, -40, -42]},
  {start_hz: 1, bin_hz: 1, bins: [-45, -75, -76]},
  {start_hz: 5, bin_hz: 1, bins: [-73]}
], 0, 5, 3).power`));
assert.ok(Math.abs(stitched[1] - 10 ** (-45 / 10)) < 1e-12);
assert.ok(stitched[2] > 10 ** (-75 / 10) && stitched[2] < 10 ** (-40 / 10));
assert.equal(stitched[4], 0);
assert.deepEqual(Array.from(go('averageSweepPower(new Float64Array([1, 0]), new Float64Array([9, 4]))')), [7.4, 4]);

// The cursor reads the nearest measured bin, using calibration at the cursor frequency.
go(`state.sweep={range:[100e6,102e6],binHz:1e6,power:new Float64Array([1e-4,1e-5,0]),settings:{gain:40,bits:8}};
state.calibration={points:[{hz:100e6,measured_dbfs:-50,actual_dbm:-30,settings:{gain:40,bits:8}},{hz:102e6,measured_dbfs:-50,actual_dbm:-20,settings:{gain:40,bits:8}}]}`);
handlers['#sweep-canvas:mousemove']({clientX:200,clientY:100});
assert.equal(elem('#sweep-tooltip').textContent,'101.000 MHz\n-50.0 dBFS (-25.0 dBm)');
assert.equal(elem('#sweep-tooltip').hidden,false);
go('state.sweep.power[1]=1e-4;renderSweep()');
assert(elem('#sweep-tooltip').textContent.includes('-40.0 dBFS (-15.0 dBm)'));
go('state.sweep.settings={gain:41,bits:8};updateSweepTooltip()');
assert(!elem('#sweep-tooltip').textContent.includes('dBm'));
handlers['#sweep-canvas:mousemove']({clientX:400,clientY:100});assert.equal(elem('#sweep-tooltip').hidden,true);
handlers['#sweep-canvas:mouseleave']();assert.equal(elem('#sweep-tooltip').hidden,true);

// A partial pass updates covered bins while retaining the last completed sweep.
go(`state.sweep = {pending: [], range: [0, 1], stepHz: 1};
const sweepTestPeek = {total: 2, bin_hz: 1, start_hz: 0};
collectSweepPeek({...sweepTestPeek, index: 0, bins: [-40, -40]});
collectSweepPeek({...sweepTestPeek, index: 1, bins: [-40, -40]});`);
assert.ok(Math.abs(go('state.sweep.power[0]') - 1e-4) < 1e-12);
go('collectSweepPeek({...sweepTestPeek, index: 0, bins: [-30, -30]})');
assert.ok(Math.abs(go('state.sweep.power[0]') - 1e-4) < 1e-12);
assert.ok(Math.abs(go('state.sweep.displayPower[0]') - 8.2e-4) < 1e-12);
go('collectSweepPeek({...sweepTestPeek, index: 1, bins: [-30, -30]})');
assert.ok(Math.abs(go('state.sweep.power[0]') - 8.2e-4) < 1e-12);
assert.equal(elem('#sweep-progress-bar').value, 100);
assert.equal(go('sweepWaterfall.rows.length'), 2, 'one waterfall row per completed sweep');
go("beginSweep({job:3, start_mhz:2400, end_mhz:2421, stop_mhz:2425, step_mhz:5, steps:6})");
go("collectSweepPeek({job:2, index:0, total:6})");
assert.equal(go('state.sweep.pending.length'),0);
elem('#sweep-bw').value='20';elem('#sweep-time').value='75';handlers['#sweep-time:change']();
assert.equal(go('sentSweep.op'),'sweep');assert.equal(go('sentSweep.dwell_ms'),75);
elem('#sweep-repeat').checked=true;handlers['#sweep-repeat:change']();
assert.equal(go('sentSweep.repeat'),true);
elem('#sweep-overlap').value='20';go('sentSweep=null');handlers['#sweep-overlap:change']();assert.equal(go('sentSweep'),null);
elem('#start-sweep').onclick();assert.equal(go('sentSweep.op'),'stop_sweep');
go("handle({event:'sweep.done', data:{job:3,state:'cancelled',seconds:1}})");
elem('#sweep-bw').value='12';elem('#sweep-overlap').value='12';go('sentSweep=null');elem('#start-sweep').onclick();assert.equal(go('sentSweep'),null);
console.log('mocked UI: sweep edits restart the active job and ignore stale tiles');

ctx.localStorage = {getItem(){return savedPreferences}, setItem(_key,value){savedPreferences=value}};
elem('#sweep-gain').value='37';elem('#sweep-bits').value='10';
elem('#sweep-overlap').value='6';handlers['#sweep-overlap:change']();
elem('#sweep-gain').value='40';elem('#sweep-bits').value='8';elem('#sweep-overlap').value='2';elem('#sweep-repeat').checked=false;
go(`restorePreferences();state.status=null;applyStatus({mode:'sweep', sweep:{job:4,start_mhz:2400,end_mhz:2421,stop_mhz:2424,step_mhz:4,steps:7},
  config:{freq_mhz:2437,rate_hz:40000000,bw_mhz:0,n:16380,bits:8,gain:22}});`);
assert.equal(elem('#sweep-gain').value,'37');assert.equal(elem('#sweep-bits').value,'10');
assert.equal(elem('#sweep-overlap').value,'6');assert.equal(elem('#sweep-repeat').checked,true);
assert.equal(go('state.sweep.job'),4);assert.equal(go('state.sweep.range[1]'),2421e6);
console.log('mocked UI: sweep settings survive refresh and rejoin an active sweep');

// Raw plots use only samples attached to the selected frame, never corrected symbols.
go("state.selectedFrame={capture_id:80,fcs_ok:true,constellation:[[1,0]],training_constellation:[[1,0]],raw_data_iq:[[0.2,0.3]],raw_training_iq:[[0.1,0.4]]};state.points='frame';renderConstellation()");
assert.equal(elem('#raw-data-empty').hidden,true);assert.equal(elem('#training-empty').hidden,true);
go("state.selectedFrame={capture_id:81,fcs_ok:true,constellation:[[1,0]],training_constellation:[[1,0]]};renderConstellation()");
assert.equal(elem('#raw-data-empty').hidden,false);assert.equal(elem('#training-empty').hidden,false);

// Calibration matches gain and bit depth regardless of other capture settings.
assert(go("sameCalibrationSettings({gain:40,bits:8,rate_hz:16000000,bw_mhz:13},{gain:40,bits:8,rate_hz:40000000,bw_mhz:0})"));
assert(!go("sameCalibrationSettings({gain:40,bits:8},{gain:40,bits:10})"));
assert(!go("sameCalibrationSettings({gain:40,bits:8},{gain:41,bits:8})"));
console.log('mocked UI: calibration matches gain and bit depth');

// Winject slot lengths are packed LSB-first into address 1 and address 2.
const packet = Buffer.alloc(24 + 3 + 2);
packet[0] = 8;
let packed = 1n | (3n << 1n) | (2n << 23n);
for (let i = 0; i < 12; i++) { packet[4 + i] = Number(packed & 255n); packed >>= 8n; }
Buffer.from([0xca,0xfe,0xba,0xbe,0x12,0x34]).copy(packet,16);
Buffer.from([7,0xaa,0xbb,9,0xcc]).copy(packet,24);
ctx.testPacket = packet.toString('hex');
let decoded = go('decodeWinject({fcs_ok:true, mpdu_hex:testPacket})');
assert.equal(decoded.domain, 0x1234);
assert.deepEqual(Array.from(decoded.slots, s => s.size), [3,0,2,0,0]);
assert.equal(decoded.slots[0].bus, 7); assert.equal(decoded.slots[2].bus, 9);
assert.equal(decoded.slots[2].offset, 27);
assert.equal(go('frameSize({fcs_ok:true,mpdu_hex:testPacket})'), 33);
assert.equal(go('decodeWinject({fcs_ok:true,mpdu_hex:testPacket.slice(0,-2)})'), null);
assert.equal(go('decodeWinject({fcs_ok:false,mpdu_hex:testPacket})'), null);
assert.equal(go("decodeWinject({fcs_ok:true,mpdu_hex:testPacket.slice(0,32)+'00000000'+testPacket.slice(40)})"), null);
console.log('mocked UI: winject domain, slot sizes, buses and malformed frames');

// Waterfall follows at the top, anchors older history, and preserves it on retune.
const waterfallScroll = elem('#waterfall-scroll');
waterfallScroll.clientWidth=4; waterfallScroll.clientHeight=6; waterfallScroll.scrollTop=0;
context2d.createImageData=(w,h)=>({data:new Uint8ClampedArray(w*h*4)});
let waterfallImage; context2d.putImageData=image=>{waterfallImage=image};
go("addWaterfallCapture({spectrum:{start_hz:100,bin_hz:1,db:[-100,0]}})");
assert.equal(waterfallScroll.scrollTop,0); assert.deepEqual([...waterfallImage.data.slice(0,4)],[0,8,40,255]);
assert.deepEqual([...waterfallImage.data.slice(12,16)],[235,30,20,255]);
waterfallScroll.scrollTop=5;
go("addWaterfallCapture({spectrum:{start_hz:100,bin_hz:1,db:[-50,-50]}})");
assert.equal(waterfallScroll.scrollTop,8); assert.equal(go('waterfall.rows.length'),2);
waterfallScroll.scrollTop=0;
go("addWaterfallCapture({spectrum:{start_hz:101,bin_hz:1,db:[-50,-50]}})");
assert.equal(waterfallScroll.scrollTop,0); assert.equal(go('waterfall.rows.length'),3);
// On the oldest row, the old high bin moves left and uncovered frequencies are blank.
waterfallScroll.scrollTop=6; go('renderWaterfall()');
assert.deepEqual([...waterfallImage.data.slice(0,4)],[235,30,20,255]);
assert.deepEqual([...waterfallImage.data.slice(12,16)],[0,8,40,255]);
go("addWaterfallCapture({spectrum:{start_hz:100,bin_hz:1,db:[-50,-50]}})");
assert.equal(go('waterfall.rows.length'),4);
assert.equal(go('waterfall.rows[3].values[0]'),-100, 'history survives tuning back');
console.log('mocked UI: waterfall colors, scroll anchoring and frequency-aligned retuning');
