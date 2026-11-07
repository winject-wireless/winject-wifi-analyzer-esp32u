const fs=require('fs'),vm=require('vm'),assert=require('assert');
const store={}; const context2d={clearRect(){},setTransform(){},beginPath(){},moveTo(){},lineTo(){},stroke(){},arc(){},fill(){},closePath(){},fillText(){},createLinearGradient(){return {addColorStop(){}}}};
function elem(id){if(!store[id])store[id]={id,textContent:'',value:'',hidden:false,dataset:{},classList:{add(){},remove(){},toggle(){}},getBoundingClientRect(){return {width:400,height:240}},getContext(){return context2d},replaceChildren(){},appendChild(){},addEventListener(){}};return store[id]}
class WS {static OPEN=1;constructor(){this.readyState=1}send(){}}
let now=10000;
const ctx={console,document:{querySelector:elem,querySelectorAll(){return []},createElement:elem},WebSocket:WS,location:{protocol:'http:',host:'x'},devicePixelRatio:1,performance:{now:()=>now},setTimeout(){},clearTimeout(){},addEventListener(){}};
vm.createContext(ctx);vm.runInContext(fs.readFileSync('web/app.js','utf8'),ctx);
function go(s){return vm.runInContext(s,ctx)}
go("state.status={mode:'live'}; state.live={points:[[1,1]],frequency_hz:2412000000,sample_rate_hz:40000000}; state.liveAt=performance.now(); renderConstellation()");
assert.equal(elem('#c-title').textContent,'Corrected constellation');assert.equal(elem('#c-points').textContent,'—');
go("onFrames({history:true,frames:[{capture_id:1,start_sample:7,detection:'ofdm_preamble_candidate',constellation:[[1,0]],training_evm_percent:9,channel_estimate:[[1,1,0]]}]});renderConstellation()");assert.equal(elem('#c-points').textContent,'—','history must not be shown as live');
go("onFrames({frames:[{capture_id:2,start_sample:8,detection:'ofdm_preamble_candidate',constellation:[[1,0]],training_evm_percent:9,channel_estimate:[[1,1,0]]}]})");assert.equal(elem('#c-points').textContent,1);assert.equal(elem('#c-evm').textContent,'9.00 %');assert(elem('#channel-meta').textContent.includes('capture 2'));
now+=4000;go('renderConstellation();renderChannelResponse()');assert.equal(elem('#c-points').textContent,'—');assert.equal(elem('#channel-meta').textContent,'No current channel estimate');
go("state.selectedFrame=state.frames[1];selectPoints('frame');renderChannelResponse()");assert.equal(elem('#c-points').textContent,1);assert(elem('#channel-meta').textContent.includes('Selected'));
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
