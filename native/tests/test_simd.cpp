#include "../simd.hpp"
#include <cassert>
#include <cmath>
#include <iostream>
#include <random>
#include <vector>
int main() {
    {
        // Two signed 10-bit I/Q words packed into five little-endian bytes.
        std::uint64_t w=(std::uint64_t(511)|(std::uint64_t(512)<<10)) |
                         ((std::uint64_t(1023)|(std::uint64_t(1)<<10))<<20);
        std::uint8_t bytes[5]; for(int i=0;i<5;++i) bytes[i]=(w>>(8*i))&255;
        std::complex<float> out[2]; winject::simd::unpack_iq10(bytes,out,2);
        assert(out[0].real()==1.0f && std::abs(out[0].imag()-512.0f/511)<1e-6f);
        assert(std::abs(out[1].real()+1.0f/511)<1e-6f && std::abs(out[1].imag()+1.0f/511)<1e-6f);
    }
    std::mt19937 rng(42); std::uniform_int_distribution<int> dist(-128,127);
    for (size_t n=0;n<1000;++n) {
        std::vector<int8_t> raw(2*n); for(auto& x:raw)x=static_cast<int8_t>(dist(rng));
        std::vector<std::complex<float>> a(n),b(n);
        winject::simd::unpack_iq8(raw.data(),a.data(),n); winject::simd::unpack_iq8_scalar(raw.data(),b.data(),n);
        for(size_t i=0;i<n;++i) assert(a[i]==b[i]);
        auto x=winject::simd::power(a.data(),n),y=winject::simd::power_scalar(a.data(),n);
        assert(std::abs(x.sum-y.sum)<1e-3); assert(std::abs(x.real_sum-y.real_sum)<1e-3);
        assert(std::abs(x.imag_sum-y.imag_sum)<1e-3); assert(x.peak==y.peak);assert(x.clipped==y.clipped);
        std::vector<uint8_t> packed((n*20+7)/8); for(auto& v:packed)v=static_cast<uint8_t>(dist(rng));
        winject::simd::unpack_iq10(packed.data(),a.data(),n);
        winject::simd::unpack_iq10_scalar(packed.data(),b.data(),n);
        for(size_t i=0;i<n;++i) assert(std::abs(a[i]-b[i])<2e-7f);
    }
    std::cout<<"SIMD conversion and power match scalar: "<<winject::simd::backend()<<'\n';
}
