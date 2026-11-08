#include "../mpdu.hpp"
#include <cassert>
#include <cstdint>
#include <iostream>
#include <vector>
#include <zlib.h>
using namespace winject;
static std::vector<uint8_t> with_fcs(std::vector<uint8_t> b){uint32_t crc=crc32(0,b.data(),b.size());for(int i=0;i<4;++i)b.push_back((crc>>(8*i))&255);return b;}
int main(){
 std::vector<uint8_t> beacon={0x80,0,0,0,255,255,255,255,255,255,0x10,0x20,0x30,0x40,0x50,0x60,0xaa,0xbb,0xcc,0xdd,0xee,0xff,0x31,0x12};
 auto framed=with_fcs(beacon);auto r=parse_mpdu_with_fcs(framed);assert(r);assert((*r)["addr3"]=="aa:bb:cc:dd:ee:ff");assert((*r)["sequence"]==0x123);assert((*r)["fragment"]==1);
 framed[8]^=1;assert(!parse_mpdu_with_fcs(framed));
 auto ack=with_fcs({0xd4,0,0,0,0x10,0x20,0x30,0x40,0x50,0x60});r=parse_mpdu_with_fcs(ack);assert(r&&(*r)["addr2"].is_null());ack.pop_back();assert(!parse_mpdu_with_fcs(ack));
 std::vector<uint8_t> data={0x88,0x03,0,0,1,2,3,4,5,6,0x11,0x12,0x13,0x14,0x15,0x16,0x21,0x22,0x23,0x24,0x25,0x26,0,0,0x31,0x32,0x33,0x34,0x35,0x36,0,0};
 r=parse_mpdu_with_fcs(with_fcs(data));assert(r&&(*r)["header_length"]==32&&(*r)["addr4"]=="31:32:33:34:35:36");data.pop_back();assert(!parse_mpdu_with_fcs(with_fcs(data)));
 beacon[0]=0x81;assert(!parse_mpdu_with_fcs(with_fcs(beacon)));
 std::cout<<"C++ MPDU FCS, addresses, truncation and control cases passed\n";
}
