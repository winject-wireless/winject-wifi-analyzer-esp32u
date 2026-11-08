#include "wjiq.hpp"
#include "simd.hpp"
#include <algorithm>
#include <cstring>
#include <zlib.h>
namespace winject {
static uint16_t le16(const uint8_t* p) { return uint16_t(p[0]) | uint16_t(p[1])<<8; }
static uint32_t le32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1])<<8 | uint32_t(p[2])<<16 | uint32_t(p[3])<<24; }
static int sext10(uint32_t x) { x &= 1023; return x & 512 ? int(x)-1024 : int(x); }
std::vector<std::complex<float>> Capture::samples() const {
    std::vector<std::complex<float>> out(count);
    if (bits==8) simd::unpack_iq8(reinterpret_cast<const int8_t*>(payload.data()),out.data(),count);
    else simd::unpack_iq10(payload.data(),out.data(),count);
    return out;
}
std::optional<Capture> Assembler::add(const uint8_t* b, size_t n) {
    auto reject=[&]()->std::optional<Capture>{++errors;return {};};
    if (n<44 || std::memcmp(b,"WJIQ",4) || b[4]!=2) return reject();
    Capture h; h.flags=b[5]; h.bits=b[6];h.rate_code=b[7];h.id=le32(b+8);h.count=le32(b+12);
    const uint32_t offset=le32(b+16);const uint16_t len=le16(b+20), hdr=le16(b+22);
    h.frequency_hz=uint64_t(le32(b+28))*1000;h.timestamp_us=le32(b+32);h.gain=b[36];h.bandwidth=b[37];
    h.job=le16(b+38);h.step=le16(b+40);h.steps=le16(b+42);
    if ((h.bits!=8 && h.bits!=10) || (h.rate_code!=0 && h.rate_code!=1 && h.rate_code!=6) || !h.count || h.count>16380 || hdr<44 || hdr>n || len!=n-hdr) return reject();
    const size_t total = h.bits==8 ? size_t(h.count)*2 : (size_t(h.count)*20+7)/8;
    if (offset>total || len>total-offset || crc32(0,b+hdr,len)!=le32(b+24)) return reject();
    auto it=partial_.find(h.id);
    if (it==partial_.end()) {
        while (order_.size()>=8) {partial_.erase(order_.front()); order_.pop_front();++lost;}
        Partial part;part.cap=h;part.bytes.resize(total);part.seen.resize(total);
        it=partial_.emplace(h.id,std::move(part)).first;order_.push_back(h.id);
    }
    auto& p=it->second;
    if (p.cap.count!=h.count || p.cap.bits!=h.bits || p.cap.rate_code!=h.rate_code || p.cap.frequency_hz!=h.frequency_hz || p.cap.job!=h.job || p.cap.step!=h.step || p.cap.steps!=h.steps) return reject();
    for (size_t i=0;i<len;i++) {
        size_t j=offset+i;
        if (p.seen[j] && p.bytes[j]!=b[hdr+i]) return reject();
    }
    for (size_t i=0;i<len;i++) {size_t j=offset+i;if (!p.seen[j]) {p.seen[j]=1;p.bytes[j]=b[hdr+i];++p.received;}}
    if (h.flags&1) p.cap.flags=h.flags;
    if (p.received!=total) return {};
    Capture result=std::move(p.cap);result.payload=std::move(p.bytes);
    partial_.erase(it);order_.erase(std::find(order_.begin(),order_.end(),h.id));++completed;
    return result;
}
}
