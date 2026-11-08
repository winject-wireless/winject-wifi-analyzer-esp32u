#include "mpdu.hpp"
#include <cstdio>
#include <string>
#include <zlib.h>
namespace winject {
using json=nlohmann::json;
static std::string hex(const std::vector<uint8_t>& bytes) {
    static constexpr char h[]="0123456789abcdef"; std::string out;out.reserve(bytes.size()*2);
    for(auto v:bytes){out.push_back(h[v>>4]);out.push_back(h[v&15]);}return out;
}
static std::string address(const std::vector<uint8_t>& b,size_t o) {
    char str[18];std::snprintf(str,sizeof str,"%02x:%02x:%02x:%02x:%02x:%02x",b[o],b[o+1],b[o+2],b[o+3],b[o+4],b[o+5]);return str;
}
std::optional<json> parse_verified_mpdu(const std::vector<uint8_t>& b) {
    if(b.size()<10)return {};
    unsigned fc=unsigned(b[0])|(unsigned(b[1])<<8),type=(fc>>2)&3,sub=(fc>>4)&15;
    if(fc&3)return {};
    bool to=fc&0x100,from=fc&0x200;size_t h=0;
    if(type==0) h=24+((fc&0x8000)?4:0);
    else if(type==1) {
        static constexpr int lengths[16]={0,0,0,0,0,0,0,0,20,28,16,16,10,10,16,16};
        h=lengths[sub]; if(!h)return {};
    } else if(type==2) {
        h=24+(to&&from?6:0);if(sub&8)h+=2+((fc&0x8000)?4:0);
    } else return {};
    if(b.size()<h)return {};
    json r={{"mpdu_hex",hex(b)},{"fcs_ok",true},{"frame_type",type},{"frame_subtype",sub},
        {"to_ds",to},{"from_ds",from},{"protected",bool(fc&0x4000)},{"header_length",h},
        {"addr1",address(b,4)},{"addr2",nullptr},{"addr3",nullptr},{"addr4",nullptr}};
    if(type!=1||h>=16)r["addr2"]=address(b,10);
    if(type==0||type==2){r["addr3"]=address(b,16);unsigned sc=unsigned(b[22])|(unsigned(b[23])<<8);r["sequence"]=sc>>4;r["fragment"]=sc&15;}
    if(type==2&&to&&from)r["addr4"]=address(b,24);
    return r;
}
std::optional<json> parse_mpdu_with_fcs(const std::vector<uint8_t>& b) {
    if(b.size()<14)return {};
    size_t n=b.size()-4;uint32_t expected=uint32_t(b[n])|(uint32_t(b[n+1])<<8)|(uint32_t(b[n+2])<<16)|(uint32_t(b[n+3])<<24);
    if(crc32(0,b.data(),n)!=expected)return {};
    return parse_verified_mpdu(std::vector<uint8_t>(b.begin(),b.begin()+n));
}
}
