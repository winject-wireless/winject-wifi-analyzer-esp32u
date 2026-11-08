#include "dsss_decoder.h"
#include <chrono>
#include <complex>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>
#include <algorithm>

static std::vector<std::complex<float>> load(const std::string& path) {
    std::ifstream f(path,std::ios::binary);if(!f)throw std::runtime_error("missing fixture "+path);
    std::vector<int8_t> bytes((std::istreambuf_iterator<char>(f)),{});std::vector<std::complex<float>> iq;
    for(size_t i=0;i+1<bytes.size();i+=2)iq.emplace_back(float(bytes[i])/127.75f,float(bytes[i+1])/127.75f);return iq;
}
int main(int argc,char** argv) {
    const std::string root=argc>1?argv[1]:"tests/fixtures/libcrafter";
    struct Case {const char* name; unsigned rate; unsigned size; bool valid;};
    const Case cases[]={
        {"dsss-10-long-clean-48",1000000,48,true},{"dsss-20-long-clean-48",2000000,48,true},
        {"dsss-55-long-clean-48",5500000,48,true},{"dsss-110-short-clean-48",11000000,48,true},
        {"dsss-10-long-impaired-64",1000000,64,true},{"dsss-20-long-impaired-64",2000000,64,true},
        {"dsss-55-long-impaired-64",5500000,64,true},{"dsss-110-long-impaired-64",11000000,64,true},
        {"dsss-110-long-channel_echo-64",11000000,64,true},{"dsss-110-short-channel_echo-64",11000000,64,true},
        {"dsss-10-long-bad_fcs-48",1000000,48,false},{"dsss-55-long-bad_fcs-48",5500000,48,false},
        {"dsss-110-short-bad_fcs-48",11000000,48,false},{"dsss-10-long-bad_crc-48",1000000,48,false},
        {"dsss-10-long-clean-48-40msps",1000000,48,true},{"dsss-20-long-clean-48-40msps",2000000,48,true},
        {"dsss-55-long-clean-48-40msps",5500000,48,true},{"dsss-110-short-clean-48-40msps",11000000,48,true},
        {"dsss-110-long-channel_echo-64-40msps",11000000,64,true},{"dsss-110-short-bad_fcs-48-40msps",11000000,48,false},
    };
    int fails=0;
    for(const auto& tc:cases){std::string name=tc.name;auto iq=load(root+"/"+name+".cs8");unsigned fs=name.find("-40msps")!=std::string::npos?40000000:20000000;
        auto frames=winject::decode_dsss(iq.data(),iq.size(),fs);
        auto base=name.substr(0,name.find("-40msps"));std::ifstream expected(root+"/"+base+".mpdu",std::ios::binary);
        std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(expected)),{});
        bool found=std::any_of(frames.begin(),frames.end(),[&](const auto& f){return f.rate_bps==tc.rate&&f.bytes.size()==tc.size&&f.bytes==bytes;});
        std::cout<<name<<":";for(const auto& f:frames){std::cout<<f.rate_bps<<"/"<<f.start_sample<<"/";for(auto b:f.bytes){const char* hex="0123456789abcdef";std::cout<<hex[b>>4]<<hex[b&15];}std::cout<<",";}std::cout<<"\n";
        if(tc.valid?!found:!frames.empty()){std::cerr<<"FAIL "<<name<<" got "<<frames.size()<<" frames\n";fails++;}}
    // A physical gap must not be bridged: neither half of this frame decodes.
    auto split=load(root+"/dsss-20-long-clean-48.cs8");auto half=split.size()/2;
    if(!winject::decode_dsss(split.data(),half,20000000).empty() ||
       !winject::decode_dsss(split.data()+half,split.size()-half,20000000).empty() ||
       !winject::decode_dsss(split.data(),split.size(),16000000).empty()) {
        std::cerr<<"FAIL truncated, discontinuous, or unsupported input\n";fails++;
    }
    if(fails)return 1;std::cout<<sizeof(cases)/sizeof(cases[0])<<" independent DSSS/CCK vector cases passed\n";
}
