// Experimental independent burst-local 802.11b receiver.
// Derived in part from libcrafter crafter/src/radio/wifi/dsss/rx.rs
// Copyright (c) 2012-present Esteban Pellegrino; MIT license in
// native/LICENSE.libcrafter.
#include "dsss_decoder.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstdint>
#include <vector>
#if defined(__aarch64__) && defined(__ARM_NEON) && !defined(WINJECT_DISABLE_NEON)
#include <arm_neon.h>
#endif

namespace winject {
namespace {
using C=std::complex<float>;
constexpr float pi=3.14159265358979323846f;
constexpr int barker[11]={1,-1,1,1,-1,1,1,1,-1,-1,-1};
struct Scrambler { unsigned state=0; unsigned bit(unsigned b) { unsigned out=b^((state>>3)&1)^((state>>6)&1);state=((state<<1)|b)&255;return out; } };
uint16_t crc16(const uint8_t* p, int n) {uint16_t crc=0xffff;while(n--){crc^=*p++;for(int k=0;k<8;k++)crc=(crc>>1)^(0x8408u & -(crc&1));}return ~crc;}
uint32_t crc32(const uint8_t* p, size_t n) {uint32_t crc=~0u;while(n--){crc^=*p++;for(int k=0;k<8;k++)crc=(crc>>1)^(0xedb88320u & -(crc&1));}return ~crc;}
int quadrant(C v){if(std::abs(v.imag())>std::abs(v.real()))return v.imag()>=0?1:3;return v.real()>=0?0:2;}
C rot(float p) {return {std::cos(p),std::sin(p)};}
struct Samples {
    std::vector<C> x;
    static const std::array<std::array<float,8>,256>& kernels() {
        static const auto table=[] {
            std::array<std::array<float,8>,256> v{};
            for(int phase=0;phase<256;phase++) {float fraction=float(phase)/256,norm=0;
                for(int k=0;k<8;k++){float d=float(k-3)-fraction;float sinc=std::abs(d)<1e-6f?1.f:std::sin(pi*d)/(pi*d);v[phase][k]=sinc*(0.5f+0.5f*std::cos(pi*d/4));norm+=v[phase][k];}
                for(float& c:v[phase])c/=norm;
            }return v;
        }();return table;
    }
    C sample(double t) const {
        int base=int(std::floor(t)); if(base<3 || base+4>=int(x.size()))return {};
        int phase=std::min(255,int((t-base)*256));const auto& w=kernels()[phase];const C* p=x.data()+base-3;
#if defined(__aarch64__) && defined(__ARM_NEON) && !defined(WINJECT_DISABLE_NEON)
        auto a=vld2q_f32(reinterpret_cast<const float*>(p));
        auto b=vld2q_f32(reinterpret_cast<const float*>(p+4));
        auto wa=vld1q_f32(w.data()),wb=vld1q_f32(w.data()+4);
        return {vaddvq_f32(vmlaq_f32(vmulq_f32(a.val[0],wa),b.val[0],wb)),
                vaddvq_f32(vmlaq_f32(vmulq_f32(a.val[1],wa),b.val[1],wb))};
#else
        C sum{};for(int k=0;k<8;k++)sum+=p[k]*w[k];return sum;
#endif
    }
    std::pair<C,float> symbol(double t) const {
        C sum{};float power=0;for(int k=0;k<11;k++){C s=sample(t+(k+0.5)*20./11.);sum+=s*float(barker[k]);power+=std::norm(s);}return {sum/11.f,std::norm(sum)/std::max(11.f*power,1e-12f)};
    }
};
// Centered, normalized Kaiser-windowed anti-alias FIR; each output is mapped
// to an even source index. Edges are zero-padded, never joined across bursts.
std::vector<C> downsample40(const C* iq,size_t count) {
    static const auto filter=[](){std::array<float,81> h{};double sum=0;for(int i=0;i<81;i++){double k=i-40, d=.475*k;double sinc=std::abs(d)<1e-12?1:std::sin(pi*d)/(pi*d);double win=std::cyl_bessel_i(0.,8*std::sqrt(std::max(0.,1-k*k/1600)))/std::cyl_bessel_i(0.,8.);h[i]=float(.475*sinc*win);sum+=h[i];}for(float& v:h)v=float(v/sum);return h;}();
    std::vector<C> out((count+1)/2);
    for(size_t n=0;n<out.size();n++){
        int start=int(2*n)-40;C sum{};int j=std::max(0,-start),end=std::min(81,int(count)-start);
#if defined(__aarch64__) && defined(__ARM_NEON) && !defined(WINJECT_DISABLE_NEON)
        float32x4_t re=vdupq_n_f32(0),im=vdupq_n_f32(0);
        for(;j+4<=end;j+=4){auto v=vld2q_f32(reinterpret_cast<const float*>(iq+start+j));auto w=vld1q_f32(filter.data()+j);re=vmlaq_f32(re,v.val[0],w);im=vmlaq_f32(im,v.val[1],w);}sum={vaddvq_f32(re),vaddvq_f32(im)};
#endif
        for(;j<end;j++)sum+=iq[start+j]*filter[j];out[n]=sum;
    }return out;
}
struct Channel {std::array<C,6> h{};std::array<float,2> noise{};bool valid=false;};
struct Training {
    std::array<C,6> train{},test{};std::array<float,2> power{};int nt=0,nv=0;
    void observe(const Samples& samples,double start,float freq) {
        C gain{};for(int k=0;k<11;k++){double off=(k+.5)*20./11.;gain+=samples.sample(start+off)*rot(-freq*float(off-10))*float(barker[k]/11.);}
        if(std::norm(gain)<1e-12f)return;
        std::array<C,6> projection{};std::array<float,2> energy{};
        for(int phase=0;phase<2;phase++)for(int k=1;k<10;k++){
            double off=(k+.25+phase*.5)*20./11.;C v=samples.sample(start+off)*rot(-freq*float(off-10))*std::conj(gain)/std::norm(gain);
            energy[phase]+=std::norm(v);for(int tap=0;tap<3;tap++)projection[phase*3+tap]+=v*float(barker[k+tap-1]);
        }
        if((nt+nv)%2){for(int i=0;i<6;i++)train[i]+=projection[i];nt++;}
        else {for(int i=0;i<6;i++)test[i]+=projection[i];for(int i=0;i<2;i++)power[i]+=energy[i];nv++;}
    }
    Channel finish() const {
        Channel ch;if(nt!=8||nv!=8)return ch;
        constexpr float inv[3][3]={{10,1,1},{1,10,-1},{1,-1,10}},gram[3][3]={{9,-1,-1},{-1,9,1},{-1,1,9}};
        for(int phase=0;phase<2;phase++){
            for(int i=0;i<3;i++)for(int j=0;j<3;j++)ch.h[phase*3+i]+=train[phase*3+j]*(inv[i][j]/(88*nt));
            float residual=power[phase];for(int i=0;i<3;i++){C hi=std::conj(ch.h[phase*3+i]);residual-=2*std::real(hi*test[phase*3+i]);for(int j=0;j<3;j++)residual+=nv*gram[i][j]*std::real(hi*ch.h[phase*3+j]);}
            ch.noise[phase]=std::max(residual/(9*nv),.01f);
        }ch.valid=true;return ch;
    }
};
struct Header {bool short_preamble;unsigned rate;size_t len;double start,payload;float freq;C previous;Scrambler scrambler;Channel channel;};
bool header_length(const std::array<uint8_t,6>& h,bool sh,unsigned& rate,size_t& n){
    if(crc16(h.data(),4)!=(h[4]|unsigned(h[5])<<8))return false;
    unsigned r=h[0],svc=h[1],duration=h[2]|unsigned(h[3])<<8;
    if((svc&~0x84u)||(r!=110&&(svc&0x80))||(sh&&r==10))return false;
    switch(r){case 10:n=duration/8;break;case 20:n=duration/4;break;case 55:n=duration*11/16;break;case 110:n=duration*11/8-(svc>>7);break;default:return false;}
    if(n<14||n>4095||(n*80+r-1)/r!=duration)return false;
    if(r==110&&((11*duration-8*n>=8)!=(bool)(svc>>7)))return false;
    rate=r*100000;return true;
}
std::pair<C,unsigned> cck(const Samples& s,double start,unsigned rate,float freq) {
    std::array<C,8> chips;for(int k=0;k<8;k++){double off=(k+0.5)*20./11.;chips[k]=s.sample(start+off)*rot(-freq*float(off-80./11.));}
    C best{};unsigned bestword=0;float score=-1;unsigned count=rate==5500000?4:64;
    for(unsigned word=0;word<count;word++){
        unsigned b,c,d;if(count==4){b=2*(word&1)+1;c=0;d=2*(word>>1);}else{auto phase=[](unsigned x){return 2*(x&1)+((x>>1)&1);};b=phase(word);c=phase(word>>2);d=phase(word>>4);}
        unsigned phases[]={b+c+d,c+d,b+d,d,b+c,c,b,0};C sum{};
        for(int k=0;k<8;k++){C v;switch(phases[k]%4){case 0:v=chips[k];break;case 1:v={chips[k].imag(),-chips[k].real()};break;case 2:v=-chips[k];break;default:v={-chips[k].imag(),chips[k].real()};}sum+=v*float((k==3||k==6)?-1:1);}
        if(std::norm(sum)>score){score=std::norm(sum);best=sum/8.f;bestword=word;}
    }
    return {best,bestword};
}
std::pair<C,unsigned> cck_equalized(const Samples& s,double start,const Header& h,C prev,bool first) {
    std::array<C,16> chips;for(int j=0;j<16;j++){double off=(j*.5+.25)*20./11.;chips[j]=s.sample(start+off)*rot(-h.freq*float(off-80./11.));}
    C previous=prev*rot(h.freq*float(first?10+80./11.:160./11.));unsigned count=h.rate==5500000?4:64,bestword=0;C best{};float bestq=-1;
    for(unsigned word=0;word<count;word++){
        unsigned b,c,d;if(count==4){b=2*(word&1)+1;c=0;d=2*(word>>1);}else{auto phase=[](unsigned x){return 2*(x&1)+((x>>1)&1);};b=phase(word);c=phase(word>>2);d=phase(word>>4);}
        unsigned phases[]={b+c+d,c+d,b+d,d,b+c,c,b,0};std::array<C,8> code;
        for(int k=0;k<8;k++){switch(phases[k]%4){case 0:code[k]={1,0};break;case 1:code[k]={0,1};break;case 2:code[k]={-1,0};break;default:code[k]={0,-1};}if(k==3||k==6)code[k]=-code[k];}
        C sum{};float energy=0,power=0;
        for(int j=0;j<16;j++){int k=j/2,ph=j%2;auto v=&h.channel.h[ph*3];C expected=v[1]*code[k];if(k>0)expected+=v[0]*code[k-1];if(k<7)expected+=v[2]*code[k+1];float boundary=k==7?std::norm(v[2]):k==0?std::norm(v[0]):0;float weight=1/(std::max(h.channel.noise[ph],.01f)+boundary);sum+=chips[j]*std::conj(expected)*weight;energy+=std::norm(expected)*weight;power+=std::norm(chips[j])*weight;}
        C rr=sum*std::conj(previous);float score=std::max(rr.real()*rr.real(),rr.imag()*rr.imag())/std::max(std::norm(previous),1e-12f);float quality=score/std::max(energy*power,1e-12f);if(quality>bestq){bestq=quality;best=sum/std::max(energy,1e-12f);bestword=word;}
    }return {best,bestword};
}
bool payload(const Samples& s,const Header& input,DsssFrame& frame,bool use_channel=false) {
    auto h=input; std::vector<uint8_t> bytes(h.len,0);size_t bits=0;double t=h.payload;C previous=h.previous;
    if(h.rate<=2000000){int width=h.rate==1000000?1:2;while(bits<h.len*8){if(t+30>=s.x.size())return false;auto [sym,q]=s.symbol(t);C delta=sym*std::conj(previous)*rot(-h.freq*20);previous=sym;int quadrant_=quadrant(delta);unsigned pair=quadrant_==0?0:quadrant_==1?2:quadrant_==2?3:1;if(width==1)pair=delta.real()<0;for(int k=0;k<width;k++){bytes[bits/8]|=h.scrambler.bit((pair>>k)&1)<<(bits%8);bits++;}t+=20;}}
    else {int width=h.rate==5500000?4:8;constexpr double dur=160./11.;while(bits<h.len*8){if(t+dur+8>=s.x.size())return false;auto [sym,word]=use_channel?cck_equalized(s,t,h,previous,bits==0):cck(s,t,h.rate,h.freq);float elapsed=bits==0?float(10+dur/2):float(dur);C delta=sym*std::conj(previous)*rot(-h.freq*elapsed);int q=(quadrant(delta)-2*int((bits/width)%2)+4)%4;unsigned pair=q==0?0:q==1?2:q==2?3:1;unsigned serial=pair|(word<<2);for(int k=0;k<width;k++){bytes[bits/8]|=h.scrambler.bit((serial>>k)&1)<<(bits%8);bits++;}previous=sym;t+=dur;}}
    size_t end=bytes.size()-4;uint32_t received=bytes[end]|uint32_t(bytes[end+1])<<8|uint32_t(bytes[end+2])<<16|uint32_t(bytes[end+3])<<24;if(crc32(bytes.data(),end)!=received)return false;
    frame={std::move(bytes),h.rate,size_t(std::max(0.,h.start)),h.short_preamble};return true;
}
}
std::vector<DsssFrame> decode_dsss(const C* iq,size_t count,unsigned sample_rate_hz) {
    std::vector<DsssFrame> result;if(!iq||(sample_rate_hz!=20000000&&sample_rate_hz!=40000000)||count<128)return result;Samples s;if(sample_rate_hz==40000000)s.x=downsample40(iq,count);else s.x.assign(iq,iq+count);count=s.x.size();
    // Separate symbol-clock phases; one input burst is never joined to another.
    for(int phase=0;phase<20;phase++){
        C previous{};unsigned symbols=0,shift=0,run=0,last=0,header_bits=0;Scrambler scr;Training training;float freq=0;bool reading=false,sh=false;std::array<uint8_t,6> header{};double sfd_end=0;
        for(double t=4+phase;t+30<count;t+=20){auto [sym,q]=s.symbol(t);if(q<0.15f||std::norm(sym)<1e-8f){previous={};symbols=shift=run=last=header_bits=0;scr={};training={};header={};freq=0;reading=false;continue;}C delta=sym*std::conj(previous);previous=sym;symbols++;
            if(reading){if((header[0]==55||header[0]==110)&&header_bits>=unsigned(sh?16:32))training.observe(s,t,freq/20);C corrected=delta*rot(-freq);int qd=quadrant(corrected);unsigned bits=qd==0?0:qd==1?2:qd==2?3:1;if(!sh)bits=corrected.real()<0;int n=sh?2:1;for(int k=0;k<n;k++){header[header_bits/8]|=scr.bit((bits>>k)&1)<<(header_bits%8);header_bits++;}if(header_bits==48){unsigned rate;size_t len;if(header_length(header,sh,rate,len)){Header h{sh,rate,len,sfd_end-(sh?72.:144.)*20,t+20,freq/20,sym,scr,training.finish()};DsssFrame frame;if(payload(s,h,frame)||(h.channel.valid&&payload(s,h,frame,true))){bool dup=false;for(const auto& old:result)if(old.bytes==frame.bytes&&std::abs((long)old.start_sample-(long)frame.start_sample)<40)dup=true;if(!dup)result.push_back(std::move(frame));}}reading=false;header_bits=0;header={};training={};}continue;}
            unsigned bit=delta.real()<0;if(symbols>=4){C val=delta*(bit?-1.f:1.f);freq=.95f*freq+.05f*std::arg(val);}unsigned plain=scr.bit(bit);shift=(shift>>1)|(plain<<15);if((shift==0xf3a0||shift==0x05cf)&&run>=32){sh=shift==0x05cf;if(last==unsigned(!sh)){reading=true;sfd_end=t+20;header_bits=0;header={};}}
            unsigned oldest=shift&1;if(oldest==last)run=std::min(256u,run+1);else{last=oldest;run=1;}
        }
    }
    if(sample_rate_hz==40000000)for(auto& f:result)f.start_sample*=2;
    return result;
}
}
