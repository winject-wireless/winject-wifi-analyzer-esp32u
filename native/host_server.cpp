// Standalone C++ host service for WJIQ v2. See README for command-line usage.
#include "wjiq.hpp"
#include "simd.hpp"
#include "ofdm.hpp"
#include "mpdu.hpp"
#include "snr.hpp"
#include "dsss_decoder.h"
#include <nlohmann/json.hpp>
#include <openssl/sha.h>
#include <openssl/evp.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using json=nlohmann::json;
using Clock=std::chrono::steady_clock;
static constexpr double pi=3.14159265358979323846;
static std::string lower(std::string s) { for(char& c:s)c=char(std::tolower((unsigned char)c));return s; }
static bool send_all(int fd,const void* src,size_t n) {auto p=(const char*)src;while(n){ssize_t k=send(fd,p,n,MSG_NOSIGNAL);if(k<=0)return false;p+=k;n-=k;}return true;}
static bool recv_all(int fd,void* dst,size_t n) {auto p=(char*)dst;while(n){ssize_t k=recv(fd,p,n,0);if(k<=0)return false;p+=k;n-=k;}return true;}
struct Client {
 int fd; std::mutex send_mu; std::atomic<bool> alive{true}; explicit Client(int x):fd(x){} ~Client(){close(fd);}
 bool frame(const std::string& text,int opcode=1) {
  std::lock_guard<std::mutex> lock(send_mu);if(!alive)return false;
  std::vector<uint8_t> h{uint8_t(0x80|opcode)};size_t n=text.size();
  if(n<126)h.push_back(n);else if(n<=65535){h.push_back(126);h.push_back(n>>8);h.push_back(n);}else{h.push_back(127);for(int i=7;i>=0;--i)h.push_back(uint64_t(n)>>(8*i));}
  if(!send_all(fd,h.data(),h.size())||!send_all(fd,text.data(),text.size())){alive=false;return false;}return true;
 }
};
static void fft(std::vector<std::complex<float>>& a) {
 size_t n=a.size();for(size_t i=1,j=0;i<n;i++){size_t b=n>>1;for(;j&b;b>>=1)j^=b;j^=b;if(i<j)std::swap(a[i],a[j]);}
 for(size_t len=2;len<=n;len<<=1){std::complex<float> wlen=std::polar(1.f,float(-2*pi/len));for(size_t i=0;i<n;i+=len){std::complex<float>w(1,0);for(size_t j=0;j<len/2;j++){auto u=a[i+j],v=a[i+j+len/2]*w;a[i+j]=u+v;a[i+j+len/2]=u-v;w*=wlen;}}}
}
static double db(double p){return 10*std::log10(std::max(p,1e-12));}
static std::vector<double> spectrum(const std::vector<std::complex<float>>& x,int n,bool peak) {
 std::vector<double> p(n,0);if(x.empty())return p;std::complex<float> mean=std::accumulate(x.begin(),x.end(),std::complex<float>{})/float(x.size());
 std::vector<float>w(n);float sum=0;for(int i=0;i<n;i++){w[i]=.5f-.5f*std::cos(2*pi*i/(n-1));sum+=w[i];}
 size_t segments=0, limit=x.size()<size_t(n)?1:1+(x.size()-n)/(n/2);
 for(size_t s=0;s<limit;s++){std::vector<std::complex<float>> v(n);for(int i=0;i<n;i++){size_t k=s*(n/2)+i;if(k<x.size())v[i]=(x[k]-mean)*w[i];}fft(v);for(int i=0;i<n;i++){double q=std::norm(v[(i+n/2)%n])/(sum*sum);p[i]=peak?std::max(p[i],q):p[i]+q;}segments++;}
 if(!peak)for(auto& v:p)v/=segments;return p;
}
static json points(const std::vector<std::complex<float>>& x,size_t n) {json a=json::array();size_t start=x.size()>n?(x.size()-n)/2:0;for(size_t i=start;i<std::min(x.size(),start+n);i++)a.push_back({x[i].real(),x[i].imag()});return a;}
static json envelope(const std::vector<std::complex<float>>& x,size_t buckets){json a=json::array();size_t step=x.size()/buckets;if(!step){for(auto v:x)a.push_back(db(std::norm(v)));return a;}for(size_t b=0;b<buckets;b++){float p=0;for(size_t j=0;j<step;j++)p=std::max(p,std::norm(x[b*step+j]));a.push_back(db(p));}return a;}

class Host {
 std::string radio_ip,bind_ip,web_dir;int port,data_port,mplane_port;int ctl=-1,data=-1; sockaddr_in radio_addr{};
 std::mutex mu,control_mu,clients_mu,sweep_mu;
 std::condition_variable calibration_cv; std::deque<std::shared_ptr<winject::Capture>> calibration_queue; std::weak_ptr<Client> calibration_owner; bool calibration_cancel=false;std::vector<std::weak_ptr<Client>> clients;
 json config={{"freq_mhz",2437},{"gain","agc"},{"bw_mhz",0},{"rate_hz",40000000},{"n",16380},{"bits",8},{"nfft",1024},{"average",0.2},{"detector","average"}};
 json radio=json::object();std::string mode="idle";bool want_live=true;uint8_t request_id=0;
 std::shared_ptr<winject::Capture> latest,snapshot;std::deque<Clock::time_point> arrivals;std::vector<double> avg;
 // Live spectrum of every capture since the last UI update (sum, or max for peak hold);
 // the UI loop only sends the newest capture ~12 times a second.
 std::vector<double> spec_acc;int spec_count=0,spec_nfft=0;bool spec_peak=false;
 winject::Assembler assembler;uint64_t datagrams=0,bytes=0;std::atomic<bool> running{true};
 std::deque<std::shared_ptr<winject::Capture>> queue;std::condition_variable cv;uint64_t detector_dropped=0,detector_processed=0,decoder_attempted=0,decoder_decoded=0,decoder_dropped=0;
 struct DecodeWork { std::shared_ptr<winject::Capture> cap; std::shared_ptr<std::vector<std::complex<float>>> samples; winject::ofdm::Candidate candidate; json record; uint64_t generation; };
 std::deque<DecodeWork> decode_queue; std::condition_variable decode_cv;
 json frames=json::array(), verified=json::array(); uint64_t generation=0; std::deque<std::shared_ptr<winject::Capture>> dsss_queue; std::condition_variable dsss_cv; uint64_t dsss_attempted=0,dsss_decoded=0,dsss_dropped=0; json sweep; bool resume_live=false; std::vector<double> sweep_acc; size_t sweep_count=0; int sweep_step=-1; uint64_t sweep_generation=UINT64_MAX; Clock::time_point sweep_start;
public:
 Host(std::string r,std::string b,int p,int dp,int mp,std::string web):radio_ip(std::move(r)),bind_ip(std::move(b)),web_dir(std::move(web)),port(p),data_port(dp),mplane_port(mp){}
 std::string request(const std::string& command) {
  std::lock_guard<std::mutex> lock(control_mu);request_id=request_id%255+1;std::string out="cmd:"+std::to_string(request_id)+" "+command+"\n";
  if(send(ctl,out.data(),out.size(),0)<0)throw std::runtime_error("mplane send failed");
  char buf[8192];auto deadline=Clock::now()+std::chrono::seconds(2);
  while(Clock::now()<deadline){ssize_t n=recv(ctl,buf,sizeof(buf),0);if(n<0)break;std::string reply(buf,n), marker=":"+std::to_string(request_id)+" ";auto at=reply.find(marker);if(at==std::string::npos)continue;auto start=reply.rfind('\n',at);start=start==std::string::npos?0:start+1;if(reply.compare(start,3,"NOK")==0)throw std::runtime_error(command+": "+reply.substr(start));return reply.substr(at+marker.size());}
  throw std::runtime_error("timeout: "+command);
 }
 json info(){json a=json::object();std::istringstream input(request("iq_info"));std::string p;while(input>>p){auto k=p.find('=');if(k!=std::string::npos)a[p.substr(0,k)]=p.substr(k+1);}return a;}
 void open_radio(){
  ctl=socket(AF_INET,SOCK_DGRAM,0);if(ctl<0)throw std::runtime_error("control socket");radio_addr.sin_family=AF_INET;radio_addr.sin_port=htons(mplane_port);if(inet_pton(AF_INET,radio_ip.c_str(),&radio_addr.sin_addr)!=1||connect(ctl,(sockaddr*)&radio_addr,sizeof(radio_addr)))throw std::runtime_error("invalid radio IP");
  timeval tv{2,0};setsockopt(ctl,SOL_SOCKET,SO_RCVTIMEO,&tv,sizeof(tv));sockaddr_in local{};socklen_t sz=sizeof(local);getsockname(ctl,(sockaddr*)&local,&sz);
  data=socket(AF_INET,SOCK_DGRAM,0);int one=1,rcv=8<<20;setsockopt(data,SOL_SOCKET,SO_REUSEADDR,&one,sizeof(one));setsockopt(data,SOL_SOCKET,SO_RCVBUF,&rcv,sizeof(rcv));sockaddr_in a{};a.sin_family=AF_INET;a.sin_port=htons(data_port);a.sin_addr.s_addr=INADDR_ANY;if(bind(data,(sockaddr*)&a,sizeof(a)))throw std::runtime_error("data bind failed");ip_mreq m{};inet_pton(AF_INET,"239.255.53.1",&m.imr_multiaddr);m.imr_interface=local.sin_addr;if(setsockopt(data,IPPROTO_IP,IP_ADD_MEMBERSHIP,&m,sizeof(m)))throw std::runtime_error("multicast join failed");
 }
 std::string dst(){return "239.255.53.1:"+std::to_string(data_port);}
 void apply(){json c;{std::lock_guard<std::mutex>l(mu);c=config;}std::string gain=c["gain"].is_string()?c["gain"].get<std::string>():std::to_string(c["gain"].get<int>());request("iq_set freq="+std::to_string(c["freq_mhz"].get<int>())+" gain="+gain+" bw="+std::to_string(c["bw_mhz"].get<int>()));}
 void start_live(){json c;{std::lock_guard<std::mutex>l(mu);c=config;want_live=true;frames=json::array();verified=json::array();avg.clear();queue.clear();dsss_queue.clear();decode_queue.clear();++generation;}request("iq_stop");request("iq_capture n="+std::to_string(c["n"].get<int>())+" rate="+std::to_string(c["rate_hz"].get<int>())+" bits="+std::to_string(c["bits"].get<int>())+" count=0 dst="+dst());{std::lock_guard<std::mutex>l(mu);mode="live";}broadcast({{"event","frames.clear"},{"data",json::object()}});}
 void broadcast(const json& msg){auto text=msg.dump();std::lock_guard<std::mutex>l(clients_mu);clients.erase(std::remove_if(clients.begin(),clients.end(),[&](auto& w){auto c=w.lock();return !c || !c->frame(text);}),clients.end());}
 json status(){std::lock_guard<std::mutex>l(mu);auto now=Clock::now();while(!arrivals.empty()&&now-arrivals.front()>std::chrono::seconds(2))arrivals.pop_front();return {{"event","status"},{"data",{{"mode",mode},{"config",config},{"radio",radio},{"sweep",sweep},{"radio_ip",radio_ip},{"captures_per_s",arrivals.size()/2.0},{"datagrams",datagrams},{"mbytes",bytes/1e6},{"completed",assembler.completed},{"lost",assembler.lost},{"errors",assembler.errors},{"detector_processed",detector_processed},{"detector_dropped",detector_dropped},{"detector_supported",config["rate_hz"]==40000000},{"detector_sample_rates_hz",{40000000}},{"decoder_backend",std::string("C++ / ")+winject::simd::backend()},{"decoder_formats",{"legacy-ofdm","DSSS","CCK"}},{"decoder_rates_mbps",{1,2,5.5,11,6,9,12,18,24,36,48,54}},{"dsss_attempted",dsss_attempted},{"dsss_decoded",dsss_decoded},{"dsss_dropped",dsss_dropped},{"dsss_error",nullptr},{"detector_error",nullptr},{"decoder_error",nullptr},{"decoder_attempted",decoder_attempted},{"decoder_decoded",decoder_decoded},{"decoder_dropped",decoder_dropped}}}};}
 void data_loop(){std::vector<uint8_t> buf(65536);while(running){sockaddr_in from{};socklen_t sz=sizeof(from);ssize_t n=recvfrom(data,buf.data(),buf.size(),0,(sockaddr*)&from,&sz);if(n<=0)continue;std::lock_guard<std::mutex>l(mu);if(from.sin_addr.s_addr!=radio_addr.sin_addr.s_addr)continue;datagrams++;bytes+=n;auto cap=assembler.add(buf.data(),n);if(!cap)continue;auto p=std::make_shared<winject::Capture>(std::move(*cap));latest=p;arrivals.push_back(Clock::now());if(mode=="calibration"){if(calibration_queue.size()>=16)calibration_queue.pop_front();calibration_queue.push_back(p);calibration_cv.notify_one();continue;}if(mode=="live"&&p->rate()==40000000){if(dsss_queue.size()==4){dsss_queue.pop_front();++dsss_dropped;}dsss_queue.push_back(p);dsss_cv.notify_one();}if((mode=="live"&&p->rate()==40000000)||(mode=="sweep"&&sweep.is_object()&&p->job==sweep["job"])) {if(queue.size()==4){queue.pop_front();detector_dropped++;}queue.push_back(p);cv.notify_one();}}}
 json live(const winject::Capture& c,const json& conf){auto x=c.samples();int n=conf["nfft"];bool peak=conf["detector"]=="peak";std::vector<double> p;{std::lock_guard<std::mutex> l(mu);if(spec_count&&spec_nfft==n&&spec_peak==peak){p=spec_acc;if(!peak)for(auto& v:p)v/=spec_count;}spec_acc.clear();spec_count=0;}if(p.empty())p=spectrum(x,n,peak);double a=conf["average"];{std::lock_guard<std::mutex> l(mu);if(avg.size()!=p.size()||a<=0)avg=p;else for(size_t i=0;i<p.size();i++)avg[i]=a*avg[i]+(1-a)*p[i];p=avg;}
 json spec=json::array();for(auto v:p)spec.push_back(db(v));auto stats=winject::simd::power(x.data(),x.size());double mean=x.empty()?0:stats.sum/x.size(),pk=stats.peak,clip=x.empty()?0:double(stats.clipped)/x.size();std::complex<double> dc=x.empty()?std::complex<double>{}:std::complex<double>(stats.real_sum/x.size(),stats.imag_sum/x.size());
 auto maxpos=std::max_element(p.begin(),p.end());int tone=int(maxpos-p.begin());return {{"capture_id",c.id},{"frequency_hz",c.frequency_hz},{"sample_rate_hz",c.rate()},{"samples",c.count},{"bits",c.bits},{"gain_index",c.gain},{"agc",bool(c.flags&8)},{"spectrum",{{"start_hz",double(c.frequency_hz)-c.rate()/2.},{"bin_hz",double(c.rate())/n},{"db",spec}}},{"power",{{"mean_dbfs",db(mean)},{"peak_dbfs",db(pk)},{"dc_dbfs",db(std::norm(dc))},{"clipped",clip},{"tone_offset_hz",(tone-n/2)*double(c.rate())/n},{"tone_hz",c.frequency_hz+(tone-n/2)*double(c.rate())/n},{"tone_dbfs",db(*maxpos)}}},{"envelope",envelope(x,512)}};}
 static json candidate_record(const winject::ofdm::Candidate& f,const winject::Capture& c,const json& cfg){
  json constellation=points(f.training_constellation,f.training_constellation.size()),channel=json::array();for(size_t k=0;k<f.channel_estimate.size();k++){auto z=f.channel_estimate[k];channel.push_back({k<26?int(k)-26:int(k)-25,z.real(),z.imag()});}
  json r={{"capture_id",c.id},{"timestamp_us",c.timestamp_us},{"frequency_hz",c.frequency_hz},{"sample_rate_hz",c.rate()},{"gain_index",c.gain},{"agc",bool(c.flags&8)},{"calibration_settings",{{"gain",c.gain},{"rate_hz",c.rate()},{"nfft",cfg["nfft"]},{"samples",c.count},{"detector",cfg["detector"]},{"bw_mhz",c.bandwidth},{"bits",c.bits}}},{"start_sample",f.start_sample},{"long_start_sample",f.long_start_sample},{"detection","ofdm_preamble_candidate"},{"signal_valid",f.signal_valid},{"signal_rate_mbps",f.signal_valid?json(f.signal_rate_mbps):json(nullptr)},{"signal_length_bytes",f.signal_valid?json(f.signal_length_bytes):json(nullptr)},{"signal_modulation",f.signal_valid?json(f.signal_modulation):json(nullptr)},{"short_score",f.short_score},{"long_score",f.long_score},{"status","candidate"},{"reason",f.signal_valid?"Legacy SIGNAL parity checked; MPDU not yet FCS verified":"OFDM-like preamble candidate; SIGNAL not decoded"},{"modulation",nullptr},{"mpdu_hex",nullptr},{"evm_percent",nullptr},{"constellation",constellation},{"training_constellation",constellation},{"channel_estimate",channel},{"training_evm_percent",f.training_valid?json(f.training_evm_percent):json(nullptr)},{"channel_coherence",f.training_valid?json(f.channel_coherence):json(nullptr)},{"carrier_offset_hz",f.training_valid?json(f.carrier_offset_hz):json(nullptr)},{"training_carriers",f.training_constellation.size()},{"training_clipped_fraction",f.training_clipped_fraction},{"training_warning",f.training_valid?"Approximate L-LTF-only EVM; candidate not decoded":"L-LTF quality not sufficient for an estimate"}};
  // A frame whose SIGNAL length runs past the end of the capture can never be FCS verified.
  // Airtime from the STF start: 20 us of preamble + SIGNAL, then 4 us per data symbol.
  if(f.signal_valid&&f.signal_rate_mbps>0&&c.rate()>0){
   int dbps=4*f.signal_rate_mbps,nsym=(16+8*f.signal_length_bytes+6+dbps-1)/dbps;double frame_us=20+4.0*nsym,per_us=c.rate()/1e6;
   double captured_us=std::clamp((double(c.count)-f.start_sample)/per_us,0.0,frame_us);bool partial=captured_us<frame_us-0.5;
   r["frame_duration_expected_us"]=frame_us;r["captured_us"]=captured_us;r["partial"]=partial;
   if(partial){int symbols=std::max(0,int((captured_us-20)/4));int bytes=std::clamp((symbols*dbps-16)/8,0,f.signal_length_bytes);
    r["captured_bytes_estimate"]=bytes;r["status"]="partial";
    r["reason"]="Partial frame: the capture ends "+std::to_string(int(frame_us-captured_us+0.5))+" us before the frame does (about "+std::to_string(bytes)+" of "+std::to_string(f.signal_length_bytes)+" B captured); it cannot be FCS verified";}
  }
  return r;
 }
 void detect_loop(){while(running){std::shared_ptr<winject::Capture> c;uint64_t gen;json capture_config;{std::unique_lock<std::mutex>l(mu);cv.wait(l,[&]{return !queue.empty()||!running;});if(!running)return;c=queue.front();queue.pop_front();gen=generation;capture_config=config;if(mode=="sweep"){l.unlock();sweep_capture(*c,gen);continue;}}auto samples=std::make_shared<std::vector<std::complex<float>>>(c->samples());auto candidates=winject::ofdm::detect(*samples,c->rate());{std::lock_guard<std::mutex>l(mu);if(gen!=generation||mode!="live")continue;detector_processed++;}
  for(auto& candidate:candidates){auto record=candidate_record(candidate,*c,capture_config);{std::lock_guard<std::mutex>l(mu);if(gen!=generation||mode!="live")break;frames.push_back(record);while(frames.size()>200)frames.erase(frames.begin());}broadcast({{"event","frames"},{"data",{{"frames",json::array({record})}}}});if(candidate.signal_valid){std::lock_guard<std::mutex>l(mu);if(gen!=generation||mode!="live")break;if(decode_queue.size()>=8){decode_queue.pop_front();++decoder_dropped;}decode_queue.push_back({c,samples,candidate,record,gen});decode_cv.notify_one();}}

 }}
 void decode_loop(){while(running){DecodeWork work;{std::unique_lock<std::mutex>l(mu);decode_cv.wait(l,[&]{return !decode_queue.empty()||!running;});if(!running)return;work=std::move(decode_queue.front());decode_queue.pop_front();if(work.generation!=generation||mode!="live")continue;}
  auto result=winject::ofdm::decode(*work.samples,work.candidate,work.cap->rate());{std::lock_guard<std::mutex>l(mu);++decoder_attempted;if(work.generation!=generation||mode!="live")continue;}if(!result)continue;auto meta=winject::parse_verified_mpdu(result->mpdu);if(!meta)continue;auto record=std::move(work.record);record.update(*meta);record["status"]="FCS verified";record["reason"]="Legacy OFDM MPDU decoded; FCS verified";record["phy"]="802.11 legacy OFDM";record["modulation"]=result->modulation;record["rate_mbps"]=result->rate_mbps;record["evm_percent"]=result->evm_valid?json(result->evm_percent):json(nullptr);record["evm_scope"]="data-field decisions including SERVICE and padding; not standards-calibrated";record["payload_clipped_fraction"]=result->clipped_fraction;record["ltf_start_sample"]=result->ltf_start_sample;record["ltf_match"]=result->ltf_match;record["constellation"]=points(result->constellation,std::min<size_t>(1024,result->constellation.size()));record["raw_data_iq"]=points(result->raw_data_iq,result->raw_data_iq.size());record["raw_training_iq"]=points(result->raw_training_iq,result->raw_training_iq.size());record["ltf_snr_db"]=std::isfinite(result->ltf_snr_db)?json(result->ltf_snr_db):json(nullptr);int start=result->ltf_start_sample-384,end=result->frame_end_sample;if(start>=0&&end<=int(work.samples->size())&&end>start){std::vector<std::complex<float>> frame(work.samples->begin()+start,work.samples->begin()+end);double sum=0,peak=0;for(auto z:frame){double p=std::norm(z);sum+=p;peak=std::max(peak,p);}auto frame_stats=winject::simd::power(frame.data(),frame.size());std::complex<double> frame_dc(frame_stats.real_sum/frame.size(),frame_stats.imag_sum/frame.size());record["frame_rssi_dbfs"]=db(std::max(0.0,frame_stats.sum/frame.size()-std::norm(frame_dc)));record["frame_clipped"]=frame_stats.clipped>0;record["frame_peak_dbfs"]=db(peak);record["frame_mean_dbfs"]=db(sum/frame.size());record["frame_duration_us"]=frame.size()*1e6/work.cap->rate();auto snr=winject::snr::estimate(*work.samples,start,end);record["frame_inband_dbfs"]=std::isfinite(snr.frame_inband)?json(db(snr.frame_inband)):json(nullptr);record["noise_floor_dbfs"]=std::isfinite(snr.noise)?json(db(snr.noise)):json(nullptr);record["snr_db"]=std::isfinite(snr.snr_db)?json(snr.snr_db):json(nullptr);record["frame_envelope_db"]=envelope(frame,256);auto spec=spectrum(frame,512,false);record["frame_spectrum_db"]=json::array();for(auto p:spec)record["frame_spectrum_db"].push_back(db(p));}{std::lock_guard<std::mutex>l(mu);if(work.generation!=generation||mode!="live")continue;++decoder_decoded;verified.push_back(record);while(verified.size()>100)verified.erase(verified.begin());}broadcast({{"event","frames"},{"data",{{"frames",json::array({record})}}}});
 }}
 void dsss_loop(){while(running){std::shared_ptr<winject::Capture> c;uint64_t gen;int spec_n=0;bool spec_pk=false,spec_on=false;{std::unique_lock<std::mutex>l(mu);dsss_cv.wait(l,[&]{return !dsss_queue.empty()||!running;});if(!running)return;c=dsss_queue.front();dsss_queue.pop_front();gen=generation;spec_n=config["nfft"];spec_pk=config["detector"]=="peak";spec_on=mode=="live";}auto samples=c->samples();if(spec_on){auto sp=spectrum(samples,spec_n,spec_pk);std::lock_guard<std::mutex>l(mu);if(spec_nfft!=spec_n||spec_peak!=spec_pk||spec_acc.size()!=sp.size()){spec_acc.assign(sp.size(),0);spec_count=0;spec_nfft=spec_n;spec_peak=spec_pk;}for(size_t i=0;i<sp.size();i++)spec_acc[i]=spec_pk?std::max(spec_acc[i],sp[i]):spec_acc[i]+sp[i];spec_count++;}auto decoded=winject::decode_dsss(samples.data(),samples.size(),c->rate());{std::lock_guard<std::mutex>l(mu);++dsss_attempted;if(gen!=generation||mode!="live")continue;}for(auto& f:decoded){auto meta=winject::parse_mpdu_with_fcs(f.bytes);if(!meta)continue;json record=*meta;record.update({{"capture_id",c->id},{"timestamp_us",c->timestamp_us},{"frequency_hz",c->frequency_hz},{"sample_rate_hz",c->rate()},{"gain_index",c->gain},{"agc",bool(c->flags&8)},{"start_sample",f.start_sample},{"detection","decoded_frame"},{"status","FCS verified"},{"reason","802.11b MPDU decoded; FCS verified"},{"phy",f.rate_bps<=2000000?"DSSS":"CCK"},{"rate_mbps",f.rate_bps/1e6},{"preamble",f.short_preamble?"short":"long"},{"modulation",f.rate_bps==1000000?"DBPSK":f.rate_bps==2000000?"DQPSK":"CCK"},{"evm_percent",nullptr},{"constellation",json::array()}});{std::lock_guard<std::mutex>l(mu);if(gen!=generation||mode!="live")break;++dsss_decoded;verified.push_back(record);while(verified.size()>100)verified.erase(verified.begin());}broadcast({{"event","frames"},{"data",{{"frames",json::array({record})}}}});}}}
 void ui_loop(){uint32_t last=0;while(running){std::this_thread::sleep_for(std::chrono::milliseconds(83));std::shared_ptr<winject::Capture> c;json cfg;std::string m;{std::lock_guard<std::mutex>l(mu);c=latest;cfg=config;m=mode;}if(c&&m=="live"&&c->id!=last){last=c->id;broadcast({{"event","live"},{"data",live(*c,cfg)}});}}}
 // A slow or timed-out radio status request must not stall the live plots.
 void info_loop(){while(running){try{auto inf=info();bool resume;{std::lock_guard<std::mutex>l(mu);radio=inf;resume=want_live&&mode!="sweep"&&mode!="calibration"&&inf.value("kind",std::string{})=="idle";}if(resume){apply();start_live();}}catch(const std::exception&e){std::lock_guard<std::mutex>l(mu);radio={{"error",e.what()}};}broadcast(status());std::this_thread::sleep_for(std::chrono::seconds(1));}}
 void end_sweep(const std::string& state,int job=-1){
  std::lock_guard<std::mutex> operation(sweep_mu);
  finish_sweep(state,job);
 }
 void finish_sweep(const std::string& state,int job=-1){
  json done;
  bool resume;
  {
   std::lock_guard<std::mutex> lock(mu);
   if(!sweep.is_object()||(job>=0&&sweep["job"]!=job))return;
   done={{"job",sweep["job"]},{"state",state},{"seconds",std::chrono::duration<double>(Clock::now()-sweep_start).count()}};
   sweep=nullptr;
   queue.clear();dsss_queue.clear();decode_queue.clear();++generation;
   resume=resume_live;
  }
  try {
   request("iq_stop");
   apply();
   if(resume)start_live();
  } catch(...) {
   std::lock_guard<std::mutex> lock(mu);
   mode="idle";
   throw;
  }
  if(!resume){
   std::lock_guard<std::mutex> lock(mu);
   mode="idle";
  }
  broadcast({{"event","sweep.done"},{"data",done}});
 }
 void start_sweep(const json& m){int start=m.at("start_mhz"),stop=m.at("stop_mhz"),step=m.value("step_mhz",10),rate=m.value("rate_hz",16000000),nf=m.value("fft",512),dwell=m.value("dwell",4),dwell_ms=m.value("dwell_ms",0),samples=m.value("samples",std::max(nf,1024)),bits=m.value("bits",8);if(samples<128||samples>16380||samples<nf||(bits!=8&&bits!=10)||(dwell_ms<0||dwell_ms>1000)||start<100||stop>6000||start>stop||step<1||step*1e6>.8*rate||(rate!=16000000&&rate!=40000000&&rate!=80000000)||nf<16||nf>16384||(nf&(nf-1))||dwell<1||dwell>100)throw std::runtime_error("invalid sweep configuration");
  const int offset=m.value("crop_offset_mhz",0);
  const int width=m.value("peek_bw_mhz",step);
  if(width<step || std::abs(double(offset))+width/2.0>0.4*rate/1e6 || start-double(offset)<100 || stop-double(offset)>6000)
   throw std::runtime_error("invalid sweep crop offset");
  const int bw=m.value("lpf_mhz",rate>=80000000?0:std::max(12,std::min(67,int(std::round(rate*.8/1e6)))));
  if(bw!=0 && (bw<12 || bw>67 || std::abs(double(offset))+width/2.0>bw/2.0))
   throw std::runtime_error("invalid sweep LPF");
  const int end=m.value("end_mhz",stop);
  if(end<start||end>stop)throw std::runtime_error("invalid sweep display range");
  const bool repeat=m.value("repeat",false), peak=m.value("detector",std::string{})=="peak";
  std::lock_guard<std::mutex> operation(sweep_mu);
  int gain;
  {
   std::lock_guard<std::mutex> lock(mu);
   gain=m.value("gain",config["gain"].is_string()?40:config["gain"].get<int>());
   if(gain<0||gain>81)throw std::runtime_error("invalid sweep gain");
   if(mode!="sweep")resume_live=mode=="live";
   queue.clear();dsss_queue.clear();decode_queue.clear();++generation;
   mode="sweep";
  }
  int job;
  try {
   request("iq_stop");
   request("iq_set gain="+std::to_string(gain)+" bw="+std::to_string(bw));
   const auto response=request("iq_sweep start="+std::to_string(start-offset)+" stop="+std::to_string(stop-offset)
    +" step="+std::to_string(step)+" n="+std::to_string(samples)+" rate="+std::to_string(rate)+" bits="+std::to_string(bits)
    +" "+(dwell_ms?"dwell_ms="+std::to_string(dwell_ms):"dwell="+std::to_string(dwell))
    +" passes="+(repeat?"0":"1")+" dst="+dst());
   const auto pos=response.find("id=");
   if(pos==std::string::npos)throw std::runtime_error("invalid sweep response");
   job=std::stoi(response.substr(pos+3));
  } catch(...) {
   try { finish_sweep("failed"); } catch(const std::exception& e) { std::cerr<<"sweep recovery: "<<e.what()<<"\n"; }
   {std::lock_guard<std::mutex> lock(mu);if(mode=="sweep")mode="idle";}
   throw;
  }
  const json info={{"job",job},{"steps",(stop-start)/step+1},{"start_mhz",start},{"end_mhz",end},{"stop_mhz",stop},{"step_mhz",step}};
  {
   std::lock_guard<std::mutex> lock(mu);
   ++generation;
   queue.clear();
   sweep=info;
   sweep["fft"]=nf;
   sweep["width_hz"]=width*1e6;
   sweep["mask_dc"]=m.contains("peek_bw_mhz");
   sweep["crop_offset_hz"]=offset*1e6;
   sweep["peak"]=peak;
   sweep_start=Clock::now();
  }
  broadcast({{"event","sweep.start"},{"data",info}});
 }
 void sweep_capture(const winject::Capture& c,uint64_t gen){json sw;{std::lock_guard<std::mutex>l(mu);if(mode!="sweep"||gen!=generation||!sweep.is_object()||c.job!=sweep["job"])return;sw=sweep;}if(sweep_generation!=gen||sweep_step!=c.step){sweep_acc.clear();sweep_count=0;sweep_generation=gen;sweep_step=c.step;}int n=sw["fft"];bool peak=sw["peak"];auto p=spectrum(c.samples(),n,peak);if(sweep_acc.size()!=p.size()){sweep_acc.assign(p.size(),0);sweep_count=0;}for(size_t i=0;i<p.size();i++)sweep_acc[i]=peak?std::max(sweep_acc[i],p[i]):sweep_acc[i]+p[i];sweep_count++;if(!(c.flags&2))return;
  double width=sw["width_hz"],bin=double(c.rate())/n,offset=sw["crop_offset_hz"];std::vector<double> values,freqs;for(int i=0;i<n;i++){double f=(i-n/2)*bin;if(std::abs(f-offset)<=width/2){values.push_back(peak?sweep_acc[i]:sweep_acc[i]/sweep_count);freqs.push_back(c.frequency_hz+f);}}sweep_acc.clear();sweep_count=0;if(values.empty())return;
  size_t group=(values.size()+255)/256,nout=values.size()/group;json bins=json::array();std::vector<double> merged_f;double power=std::accumulate(values.begin(),values.end(),0.0),max_db=-120;for(size_t i=0;i<nout;i++){double value=0,f=0;for(size_t j=0;j<group;j++){value=peak?std::max(value,values[i*group+j]):value+values[i*group+j];f+=freqs[i*group+j];}if(!peak)value/=group;double d=db(value);bins.push_back(d);max_db=std::max(max_db,d);merged_f.push_back(f/group);}json tile={{"job",c.job},{"frequency_hz",c.frequency_hz},{"start_hz",merged_f.front()},{"bin_hz",merged_f.size()>1?merged_f[1]-merged_f[0]:width},{"bins",bins},{"power_dbfs",db(power)},{"peak_dbfs",max_db},{"index",c.step},{"total",c.steps},{"gain_index",c.gain},{"calibration_settings",{{"gain",c.gain},{"rate_hz",c.rate()},{"nfft",n},{"samples",c.count},{"detector",peak?"peak":"average"},{"bw_mhz",c.bandwidth},{"bits",c.bits}}}};if(sw["mask_dc"].get<bool>())tile["dc_guard_hz"]=250000+group*bin/2;
  broadcast({{"event","sweep.tile"},{"data",tile}});if(c.flags&16)try{end_sweep("completed",c.job);}catch(const std::exception&e){std::cerr<<"sweep completion: "<<e.what()<<"\n";}
 }
 void calibrate(double power,double tone,int start_gain,int end_gain,json cfg,std::shared_ptr<Client> owner){
  json points=json::array(),skipped=json::array(); std::string error; bool cancelled=false;
  try {
   auto inf=info(); int maxgain=std::stoi(inf.at("gain_max").get<std::string>());if(end_gain==-1)end_gain=maxgain;if(maxgain<0||maxgain>255||start_gain<0||end_gain<start_gain||end_gain>maxgain)throw std::runtime_error("invalid gain range");
   request("iq_stop");
   for(int gain=start_gain;gain<=end_gain;gain++){
    {std::lock_guard<std::mutex> l(mu);if(calibration_cancel||!owner->alive){cancelled=true;break;}}
    broadcast({{"event","calibration.progress"},{"data",{{"gain",gain},{"total",end_gain-start_gain+1},{"end_gain",end_gain},{"accepted",points.size()},{"skipped",skipped.size()}}}});
    request("iq_set gain="+std::to_string(gain));
    {std::lock_guard<std::mutex> l(mu);calibration_queue.clear();}
    constexpr int attempts=9, min_inliers=3; constexpr double tolerance_db=1.5;
    auto reply=request("iq_capture n="+std::to_string(cfg["n"].get<int>())+" rate="+std::to_string(cfg["rate_hz"].get<int>())+" bits="+std::to_string(cfg["bits"].get<int>())+" count="+std::to_string(attempts)+" dst="+dst());
    auto at=reply.find("id=");if(at==std::string::npos)throw std::runtime_error("invalid capture response");int job=std::stoi(reply.substr(at+3));
    std::vector<double> levels;std::vector<uint32_t> seen;json settings;int clipped=0,weak=0,mismatch=0;auto deadline=Clock::now()+std::chrono::seconds(8);
    for(int attempt=0;attempt<attempts;attempt++){
     std::shared_ptr<winject::Capture> cap;
     {std::unique_lock<std::mutex> l(mu);while(!cap){if(calibration_cancel||!owner->alive){cancelled=true;break;}if(!calibration_queue.empty()){auto next=calibration_queue.front();calibration_queue.pop_front();if(next->job==job&&std::find(seen.begin(),seen.end(),next->id)==seen.end())cap=next;continue;}if(calibration_cv.wait_until(l,deadline)==std::cv_status::timeout)throw std::runtime_error("timed out waiting for calibration captures");}}
     if(cancelled)break;seen.push_back(cap->id);
     if((cap->flags&8)||cap->gain!=gain){mismatch++;continue;}
     auto x=cap->samples();auto stats=winject::simd::power(x.data(),x.size());if(stats.clipped){clipped++;continue;}
     int n=cfg["nfft"];auto spec=spectrum(x,n,cfg["detector"]=="peak");int k=std::lround((tone-cap->frequency_hz)/double(cap->rate())*n+n/2);
     if(k<4||k>=n-4||std::abs(k-n/2)<=4)throw std::runtime_error("tone too close to DC or spectrum edge");
     // Allow a small frequency error between the signal generator and receiver.
     // Search only within two FFT bins to avoid selecting an unrelated signal.
     const int expected=k;for(int i=expected-2;i<=expected+2;i++)if(spec[i]>spec[k])k=i;
     std::vector<double> noise;for(int i=0;i<n;i++)if(std::abs(i-expected)>5&&std::abs(i-n/2)>2)noise.push_back(spec[i]);std::nth_element(noise.begin(),noise.begin()+noise.size()/2,noise.end());
     if(db(spec[k])<db(noise[noise.size()/2])+10){weak++;continue;}
     levels.push_back(db(spec[k]));settings={{"gain",gain},{"rate_hz",cap->rate()},{"nfft",n},{"samples",cap->count},{"detector",cfg["detector"]},{"bw_mhz",cfg["bw_mhz"]},{"bits",cap->bits}};
    }
    if(cancelled)break;
    // One-dimensional RANSAC: exhaustively test each observed level as a model.
    std::vector<double> best;
    for(double model:levels){std::vector<double> inliers;for(double level:levels)if(std::abs(level-model)<=tolerance_db)inliers.push_back(level);if(inliers.size()>best.size())best=std::move(inliers);}
    if(best.size()<min_inliers||best.size()*2<levels.size()){
     skipped.push_back({{"gain",gain},{"reason","insufficient consistent measurements"},{"valid",levels.size()},{"inliers",best.size()},{"clipped",clipped},{"weak",weak},{"gain_mismatch",mismatch}});continue;
    }
    std::sort(best.begin(),best.end());double estimate=best[best.size()/2];if(best.size()%2==0)estimate=(estimate+best[best.size()/2-1])/2;
    points.push_back({{"hz",tone},{"measured_dbfs",estimate},{"actual_dbm",power},{"settings",settings},{"measurements",attempts},{"valid",levels.size()},{"inliers",best.size()}});
   }
  }catch(const std::exception& e){error=e.what();}
  try{request("iq_stop");apply();{std::lock_guard<std::mutex> l(mu);mode="idle";}start_live();}catch(const std::exception& e){error=std::string("Could not restore radio: ")+e.what();std::lock_guard<std::mutex> l(mu);mode="idle";want_live=false;}
  {std::lock_guard<std::mutex> l(mu);calibration_owner.reset();calibration_cancel=false;}
  broadcast({{"event","calibration.done"},{"data",{{"points",error.empty()&&!cancelled?points:json::array()},{"skipped",skipped},{"error",error.empty()?json(nullptr):json(error)},{"cancelled",cancelled}}}});
 }
 void command(const json& msg,std::shared_ptr<Client> owner){std::string op=msg.value("op",std::string{});
 {std::lock_guard<std::mutex>l(mu);if(mode=="calibration"){if(op=="calibration.cancel"&&calibration_owner.lock()==owner){calibration_cancel=true;calibration_cv.notify_one();return;}throw std::runtime_error("Calibration is in progress");}}
 if(op=="radio.connect") {
  std::string ip=msg.at("address"); sockaddr_in target{};target.sin_family=AF_INET;target.sin_port=htons(mplane_port);
  if(inet_pton(AF_INET,ip.c_str(),&target.sin_addr)!=1 || target.sin_addr.s_addr==INADDR_ANY) throw std::runtime_error("Enter a valid IPv4 address");
  // Leave the old analyzer idle: end a running sweep and stop its IQ stream, which would
  // otherwise keep flooding the multicast group. It may already be unreachable.
  {bool sweeping;{std::lock_guard<std::mutex> l(mu);sweeping=mode=="sweep";}if(sweeping)end_sweep("cancelled");try{request("iq_stop");}catch(const std::exception&){}}
  {std::lock_guard<std::mutex> control(control_mu);if(connect(ctl,(sockaddr*)&target,sizeof(target)))throw std::runtime_error("Could not connect control socket");}
  sockaddr_in local{};socklen_t len=sizeof(local);getsockname(ctl,(sockaddr*)&local,&len);ip_mreq membership{};inet_pton(AF_INET,"239.255.53.1",&membership.imr_multiaddr);membership.imr_interface=local.sin_addr;
  if(setsockopt(data,IPPROTO_IP,IP_ADD_MEMBERSHIP,&membership,sizeof(membership)) && errno!=EADDRINUSE) throw std::runtime_error("Could not join multicast on new interface");
  {std::lock_guard<std::mutex> l(mu);radio_addr=target;radio_ip=ip;radio={{"error","Connecting"}};mode="idle";latest.reset();++generation;}
  const char* path=getenv("WINJECT_ANALYZER_ADDRESS_FILE");if(path){std::ofstream f(path);f<<ip<<"\n";if(!f)throw std::runtime_error("Could not save analyzer address");}
 }
 else if(op=="calibration.start") {double power=msg.at("power_dbm"),tone=msg.at("tone_hz");if(!std::isfinite(power)||!std::isfinite(tone))throw std::runtime_error("invalid tone power or frequency");json cfg;{std::lock_guard<std::mutex>l(mu);if(mode!="live"||!latest)throw std::runtime_error("start live capture before calibrating");if(std::abs(tone-latest->frequency_hz)>=latest->rate()/2.)throw std::runtime_error("tone outside capture bandwidth");cfg=config;mode="calibration";calibration_owner=owner;calibration_cancel=false;calibration_queue.clear();++generation;}int start_gain=msg.value("start_gain",0),end_gain=msg.value("end_gain",-1);std::thread(&Host::calibrate,this,power,tone,start_gain,end_gain,cfg,owner).detach();}
 else if(op=="config"){
  json c;std::string m;{std::lock_guard<std::mutex>l(mu);c=config;m=mode;}for(const char* k:{"freq_mhz","gain","bw_mhz","rate_hz","n","bits","nfft","average","detector"})if(msg.contains(k))c[k]=msg[k];int rate=c["rate_hz"],n=c["n"],bits=c["bits"],nf=c["nfft"],freq=c["freq_mhz"];if((rate!=16000000&&rate!=40000000&&rate!=80000000)||n<128||n>16380||(bits!=8&&bits!=10)||nf<16||nf>16384||(nf&(nf-1))||freq<100||freq>6000)throw std::runtime_error("invalid radio configuration");{std::lock_guard<std::mutex>l(mu);config=c;}if(m!="sweep"){apply();if(m=="live")start_live();}
 }else if(op=="live"){if(msg.value("on",true)){apply();start_live();}else{{std::lock_guard<std::mutex>l(mu);want_live=false;++generation;queue.clear();dsss_queue.clear();decode_queue.clear();}request("iq_stop");std::lock_guard<std::mutex>l(mu);mode="idle";}}
 else if(op=="snapshot"){std::shared_ptr<winject::Capture> c;{std::lock_guard<std::mutex>l(mu);snapshot=latest;c=snapshot;}if(!c)throw std::runtime_error("no capture yet");auto x=c->samples();broadcast({{"event","snapshot"},{"data",{{"capture_id",c->id},{"frequency_hz",c->frequency_hz},{"sample_rate_hz",c->rate()},{"samples",c->count},{"bits",c->bits},{"envelope",envelope(x,1024)},{"download","/capture/snapshot.cf32"}}}});}
 else if(op=="sweep")start_sweep(msg);
 else if(op=="stop_sweep")end_sweep("cancelled");
 else throw std::runtime_error("unsupported command: "+op);}
 void client(int fd){timeval timeout{0,250000};setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof(timeout));auto c=std::make_shared<Client>(fd);std::string request_text;char ch;while(request_text.size()<16384&&recv(fd,&ch,1,0)==1){request_text+=ch;if(request_text.size()>=4&&request_text.substr(request_text.size()-4)=="\r\n\r\n")break;}std::istringstream req(request_text);std::string method,path,version;req>>method>>path>>version;std::string line,key;std::getline(req,line);while(std::getline(req,line)){auto pos=line.find(':');if(pos!=std::string::npos&&lower(line.substr(0,pos))=="sec-websocket-key"){key=line.substr(pos+1);while(!key.empty()&&(key.front()==' '||key.front()=='\t'))key.erase(key.begin());while(!key.empty()&&(key.back()=='\r'||key.back()==' '))key.pop_back();}}
 if(method!="GET")return;if(path=="/ws"&&!key.empty()){
  auto input=key+"258EAFA5-E914-47DA-95CA-C5AB0DC85B11";unsigned char digest[SHA_DIGEST_LENGTH],enc[64];SHA1((const unsigned char*)input.data(),input.size(),digest);EVP_EncodeBlock(enc,digest,SHA_DIGEST_LENGTH);std::string response="HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: "+std::string((char*)enc)+"\r\n\r\n";if(!send_all(fd,response.data(),response.size()))return;{std::lock_guard<std::mutex>l(clients_mu);clients.push_back(c);}c->frame(status().dump());{std::lock_guard<std::mutex>l(mu);if(!frames.empty()||!verified.empty()){json history=frames;for(auto& f:verified)history.push_back(f);c->frame(json({{"event","frames"},{"data",{{"frames",history},{"history",true}}}}).dump());}}
  while(c->alive){uint8_t h[2];if(!recv_all(fd,h,2))break;int opcode=h[0]&15;bool masked=h[1]&128;uint64_t n=h[1]&127;if(n==126){uint8_t x[2];if(!recv_all(fd,x,2))break;n=(x[0]<<8)|x[1];}else if(n==127){uint8_t x[8];if(!recv_all(fd,x,8))break;n=0;for(auto v:x)n=n*256+v;}if(!masked||n>(1<<20)||(h[0]&128)==0)break;uint8_t mask[4];if(!recv_all(fd,mask,4))break;std::string payload(n,'\0');if(!recv_all(fd,payload.data(),n))break;for(size_t i=0;i<n;i++)payload[i]^=mask[i%4];if(opcode==8)break;if(opcode==9){c->frame(payload,10);continue;}if(opcode!=1)continue;try{command(json::parse(payload),c);}catch(const std::exception&e){c->frame(json({{"event","error"},{"data",{{"message",e.what()}}}}).dump());}c->frame(status().dump());}c->alive=false;{std::lock_guard<std::mutex>l(mu);if(calibration_owner.lock()==c){calibration_cancel=true;calibration_cv.notify_one();}}return;
 }
 std::string body,type="application/octet-stream";if(path.rfind("/capture/snapshot",0)==0){std::shared_ptr<winject::Capture>s;{std::lock_guard<std::mutex>l(mu);s=snapshot;}if(s){if(path.find(".json")!=std::string::npos){type="application/json";body=json({{"capture_id",s->id},{"sample_count",s->count},{"bits",s->bits},{"frequency_hz",s->frequency_hz},{"timestamp_us",s->timestamp_us},{"gain_index",s->gain},{"agc",bool(s->flags&8)},{"bandwidth_mhz",s->bandwidth},{"sample_rate_hz",s->rate()},{"format","cf32_le"},{"scale","|x| = 1 at 0 dBFS"}}).dump();}else{auto samples=s->samples();body.assign((const char*)samples.data(),samples.size()*sizeof(samples[0]));}}}
 else{if(path=="/")path="/index.html";auto q=path.find('?');if(q!=std::string::npos)path.resize(q);if(path.find("..")!=std::string::npos)return;std::ifstream f(web_dir+path,std::ios::binary);if(f){std::ostringstream stream;stream<<f.rdbuf();body=stream.str();if(path.find(".html")!=std::string::npos)type="text/html";else if(path.find(".js")!=std::string::npos)type="text/javascript";else if(path.find(".css")!=std::string::npos)type="text/css";}}
 std::string header="HTTP/1.1 "+std::string(body.empty()?"404 Not Found":"200 OK")+"\r\nContent-Type: "+type+"\r\nCache-Control: no-cache\r\nConnection: close\r\nContent-Length: "+std::to_string(body.size())+"\r\n\r\n";send_all(fd,header.data(),header.size());send_all(fd,body.data(),body.size());
 }
 void run(){open_radio();try{radio=info();apply();start_live();}catch(const std::exception&e){radio={{"error",e.what()}};std::cerr<<e.what()<<"\n";}std::thread(&Host::data_loop,this).detach();std::thread(&Host::ui_loop,this).detach();std::thread(&Host::info_loop,this).detach();std::thread(&Host::detect_loop,this).detach();std::thread(&Host::decode_loop,this).detach();std::thread(&Host::dsss_loop,this).detach();int fd=socket(AF_INET,SOCK_STREAM,0);int one=1;setsockopt(fd,SOL_SOCKET,SO_REUSEADDR,&one,sizeof(one));sockaddr_in a{};a.sin_family=AF_INET;a.sin_port=htons(port);if(inet_pton(AF_INET,bind_ip.c_str(),&a.sin_addr)!=1||bind(fd,(sockaddr*)&a,sizeof(a))||listen(fd,32))throw std::runtime_error("HTTP listen failed");std::cerr<<"C++ host listening on "<<bind_ip<<":"<<port<<"\n";while(running){int c=accept(fd,nullptr,nullptr);if(c>=0)std::thread(&Host::client,this,c).detach();}close(fd);}
};
int main(int argc,char**argv){try{std::string radio="192.168.253.9",bind="0.0.0.0",web="web";int port=8082,data=5300,mplane=22;for(int i=1;i<argc;i++){std::string a=argv[i];if(i+1==argc)throw std::runtime_error("missing argument");std::string v=argv[++i];if(a=="--host")radio=v;else if(a=="--http-host")bind=v;else if(a=="--http-port")port=std::stoi(v);else if(a=="--data-port")data=std::stoi(v);else if(a=="--mplane-port")mplane=std::stoi(v);else if(a=="--web-dir")web=v;else throw std::runtime_error("unknown option "+a);}if(const char* path=getenv("WINJECT_ANALYZER_ADDRESS_FILE")){std::ifstream f(path);std::string saved;if(f>>saved){in_addr addr{};if(inet_pton(AF_INET,saved.c_str(),&addr)==1)radio=saved;}}Host(radio,bind,port,data,mplane,web).run();}catch(const std::exception&e){std::cerr<<"error: "<<e.what()<<"\n";return 1;} }
