const fs=require('fs'),vm=require('vm'),assert=require('assert');
const handlers={}; const store={}; const context2d={clearRect(){},setTransform(){},beginPath(){},moveTo(){},lineTo(){},stroke(){},arc(){},fill(){},closePath(){},fillText(){},createLinearGradient(){return {addColorStop(){}}}};
function elem(id){if(!store[id])store[id]={id,textContent:'',value:'',hidden:false,dataset:{},classList:{add(){},remove(){},toggle(){}},getBoundingClientRect(){return {width:400,height:240}},getContext(){return context2d},replaceChildren(){},appendChild(){},focus(){},children:[],addEventListener(type,fn){handlers[id+':'+type]=fn}};return store[id]}
class WS {static OPEN=1;constructor(){this.readyState=1}send(){}}
let now=10000;
const ctx={console,document:{querySelector:elem,querySelectorAll(){return []},createElement:elem},WebSocket:WS,location:{protocol:'http:',host:'x'},devicePixelRatio:1,performance:{now:()=>now},setTimeout(){},clearTimeout(){},addEventListener(){}};
vm.createContext(ctx);vm.runInContext(fs.readFileSync('web/app.js','utf8'),ctx);
function go(s){return vm.runInContext(s,ctx)}
go("state.status={mode:'live'}; state.live={points:[[1,1]],frequency_hz:2412000000,sample_rate_hz:40000000}; state.liveAt=performance.now(); renderConstellation()");
assert.equal(elem('#c-title').textContent,'Corrected constellation');assert.equal(elem('#c-points').textContent,'—');
go("onFrames({history:true,frames:[{capture_id:1,start_sample:7,detection:'ofdm_preamble_candidate',constellation:[[1,0]],training_evm_percent:9,channel_estimate:[[1,1,0]]}]});renderConstellation()");assert.equal(elem('#c-points').textContent,'—','history must not be shown as live');
go("onFrames({frames:[{capture_id:2,start_sample:8,detection:'ofdm_preamble_candidate',constellation:[[1,0]],training_evm_percent:9,channel_estimate:[[1,1,0]]}]})");assert.equal(elem('#c-points').textContent,'—');assert.equal(elem('#channel-meta').textContent,'No current channel estimate');
go("onFrames({frames:[{capture_id:3,start_sample:9,fcs_ok:true,phy:'OFDM',constellation:[[1,0]],training_constellation:[[1,0]],channel_estimate:[[1,1,0]]}]})");
assert.equal(elem('#c-points').textContent,1);assert(elem('#channel-meta').textContent.includes('capture 3'));
now+=4000;go("onFrames({frames:[{capture_id:4,start_sample:10,detection:'ofdm_preamble_candidate',channel_estimate:[[1,2,0]]}]});renderConstellation();renderChannelResponse()");
assert.equal(elem('#c-points').textContent,1);assert(elem('#channel-meta').textContent.includes('capture 3'));assert.equal(go('state.frames.length'),4);
go("state.selectedFrame=state.frames[1];selectPoints('frame');renderChannelResponse()");assert.equal(elem('#c-points').textContent,'—');assert(elem('#channel-meta').textContent.includes('Selected'));
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

go("renderChannelResponse()");assert.equal(elem('#channel-meta').textContent,'No OFDM subcarrier estimate for this PHY');

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
key('ArrowDown');assert(prevented);assert.equal(go('state.selectedFrame.capture_id'),21);assert.equal(elem('#frame-gain').textContent,'40 (manual; capture start)');
key('ArrowDown');assert.equal(go('state.selectedFrame.capture_id'),20);assert.equal(elem('#frame-gain').textContent,'37 (AGC; capture start)');
key('ArrowUp');assert.equal(go('state.selectedFrame.capture_id'),21);
assert(fs.readFileSync('web/index.html','utf8').includes('id="frame-gain"'));
console.log('mocked UI: frame gain and arrow navigation passed');

// Select a usable tone rather than the DC artifact and validate the chosen frequency.
go("state.status.mode='live';state.live={spectrum:{start_hz:2400000000,bin_hz:100000,db:Array(32).fill(-90)}};state.live.spectrum.db[16]=0;state.live.spectrum.db[17]=-1;state.live.spectrum.db[20]=-10;state.live.spectrum.db[23]=-20");
elem('#calibration-auto').onclick();assert.equal(elem('#calibration-auto-frequency').value,'2402.3');
elem('#calibration-auto-power').value='-60';elem('#calibration-auto-frequency').value='2401.6';
elem('#calibration-auto-form').onsubmit({preventDefault(){}});assert(elem('#calibration-auto-tone').textContent.includes('away from the center'));
console.log('mocked UI: automatic calibration avoids DC and validates tone frequency');

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
assert(go("calibrationAxisNote({...state.live,bits:10},false)").includes('bit depth differs'));
assert(go("calibrationAxisNote({...state.live,agc:true},true)").includes('AGC may change during capture'));
console.log('mocked UI: dBm availability explains missing gain and setting mismatches');

// Frame RSSI uses the frame's recorded gain and settings, not the current live capture.
go(`state.selectedFrame={capture_id:44,start_sample:10,fcs_ok:true,frequency_hz:2450000000,agc:true,frame_rssi_dbfs:-45,frame_clipped:false,calibration_settings:{gain:40,rate_hz:40000000,nfft:16,samples:128,detector:'average',bw_mhz:0,bits:8}};state.live.gain_index=17;renderFrames()`);
assert.equal(elem('#frame-dbm').textContent,'-25.0 dBm');assert(elem('#frame-rssi-note').textContent.includes('AGC may change'));
go("state.selectedFrame.frame_clipped=true;renderFrames()");assert.equal(elem('#frame-dbm').textContent,'-25.0 dBm ⚠');assert(elem('#frame-rssi-note').textContent.includes('Clipped samples'));
go("state.selectedFrame.frame_clipped=false;state.selectedFrame.calibration_settings.gain=41;renderFrames()");assert.equal(elem('#frame-dbm').textContent,'Unavailable');assert(elem('#frame-rssi-note').textContent.includes('Gain 41 is uncalibrated'));
console.log('mocked UI: frame RSSI uses captured settings and flags AGC and clipping');

// Frame peak, mean and envelope use the selected frame's calibrated offset.
go(`state.selectedFrame={capture_id:55,start_sample:0,fcs_ok:true,frequency_hz:2450000000,agc:true,frame_clipped:true,frame_rssi_dbfs:-35,frame_peak_dbfs:0,frame_mean_dbfs:-10,frame_envelope_db:[-30,-20,0],frame_duration_us:10,calibration_settings:{gain:40,rate_hz:40000000,nfft:16,samples:128,detector:'average',bw_mhz:0,bits:8}};state.points='frame';state.framePlot='envelope';renderFrames()`);
assert.equal(elem('#frame-peak').textContent,'0.00 dBFS (20.0 dBm)');assert.equal(elem('#frame-mean').textContent,'-10.00 dBFS (10.0 dBm)');
axisLabels.length=0;go('renderChannelResponse()');assert(axisLabels.some(x=>x.label.endsWith('dBFS')));assert(axisLabels.some(x=>x.label.endsWith('dBm')));assert(elem('#channel-note').textContent.includes('Samples clipped'));
axisLabels.length=0;go("state.framePlot='fft';state.selectedFrame.frame_spectrum_db=[-50,-20];renderChannelResponse()");assert(!axisLabels.some(x=>x.label.endsWith('dBm')));
go("state.selectedFrame.calibration_settings.gain=41;renderFrames();state.framePlot='envelope';renderChannelResponse()");assert.equal(elem('#frame-peak').textContent,'0.00 dBFS');assert(elem('#channel-note').textContent.includes('matching calibration'));
console.log('mocked UI: calibrated frame powers and envelope axis');
