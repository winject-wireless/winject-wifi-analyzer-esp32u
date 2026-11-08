// Common, portable IQ kernels. NEON is mandatory on AArch64; other targets use
// the scalar reference. Input/output lengths are in complex samples.
#pragma once
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#if defined(__aarch64__) && defined(__ARM_NEON)
#include <arm_neon.h>
#define WINJECT_SIMD_NEON 1
#else
#define WINJECT_SIMD_NEON 0
#endif

namespace winject::simd {
inline constexpr const char* backend() { return WINJECT_SIMD_NEON ? "arm-neon" : "scalar"; }
inline void unpack_iq8_scalar(const std::int8_t* input, std::complex<float>* output, std::size_t n) {
    constexpr float scale = 4.0f / 511.0f;
    for (std::size_t i=0; i<n; ++i) output[i] = {input[2*i]*scale, -input[2*i+1]*scale};
}
inline void unpack_iq8(const std::int8_t* input, std::complex<float>* output, std::size_t n) {
    if (!n) return;
    std::size_t i=0;
#if WINJECT_SIMD_NEON
    constexpr float scale = 4.0f / 511.0f;
    // std::complex<float> has the standard-required interleaved representation.
    for (; i+8<=n; i+=8) {
        const int8x8x2_t iq = vld2_s8(input+2*i);
        const int16x8_t re = vmovl_s8(iq.val[0]), im = vmovl_s8(iq.val[1]);
        const float32x4_t a = vmulq_n_f32(vcvtq_f32_s32(vmovl_s16(vget_low_s16(re))), scale);
        const float32x4_t b = vmulq_n_f32(vcvtq_f32_s32(vmovl_s16(vget_high_s16(re))), scale);
        const float32x4_t c = vmulq_n_f32(vcvtq_f32_s32(vmovl_s16(vget_low_s16(im))), -scale);
        const float32x4_t d = vmulq_n_f32(vcvtq_f32_s32(vmovl_s16(vget_high_s16(im))), -scale);
        float32x4x2_t low{{a,c}}, high{{b,d}};
        vst2q_f32(reinterpret_cast<float*>(output+i),low);
        vst2q_f32(reinterpret_cast<float*>(output+i+4),high);
    }
#endif
    unpack_iq8_scalar(input+2*i,output+i,n-i);
}
inline std::uint32_t iq10_word(const std::uint8_t* input, std::size_t i) {
    const std::size_t offset=(i*20)/8;
    return (std::uint32_t(input[offset]) | (std::uint32_t(input[offset+1])<<8) |
            (std::uint32_t(input[offset+2])<<16)) >> ((i & 1)*4);
}
inline void unpack_iq10_scalar(const std::uint8_t* input, std::complex<float>* output, std::size_t n) {
    for (std::size_t i=0;i<n;++i) {
        auto word=iq10_word(input,i); int re=word&1023, im=(word>>10)&1023;
        if(re&512)re-=1024;
        if(im&512)im-=1024;
        output[i]={re/511.0f,-im/511.0f};
    }
}
inline void unpack_iq10(const std::uint8_t* input, std::complex<float>* output, std::size_t n) {
    if (!n) return;
    std::size_t i=0;
#if WINJECT_SIMD_NEON
    for(;i+4<=n;i+=4) {
        std::uint32_t words[4]={iq10_word(input,i),iq10_word(input,i+1),iq10_word(input,i+2),iq10_word(input,i+3)};
        auto v=vld1q_u32(words);
        auto re=vshrq_n_s32(vreinterpretq_s32_u32(vshlq_n_u32(v,22)),22);
        auto im=vshrq_n_s32(vreinterpretq_s32_u32(vshlq_n_u32(v,12)),22);
        float32x4x2_t out{{vmulq_n_f32(vcvtq_f32_s32(re),1.0f/511.0f),vmulq_n_f32(vcvtq_f32_s32(im),-1.0f/511.0f)}};
        vst2q_f32(reinterpret_cast<float*>(output+i),out);
    }
#endif
    // Four samples always end on a byte boundary.
    unpack_iq10_scalar(input+(i*20)/8,output+i,n-i);
}
struct Power {
    double sum=0, real_sum=0, imag_sum=0;
    float peak=0;
    std::size_t clipped=0;
};
inline Power power_scalar(const std::complex<float>* samples, std::size_t n, float clip=501.0f/511.0f) {
    Power r;
    for (std::size_t i=0;i<n;++i) {
        float x=samples[i].real(), y=samples[i].imag(), p=x*x+y*y;
        r.sum+=p; r.real_sum+=x; r.imag_sum+=y; r.peak=std::max(r.peak,p);
        r.clipped += (std::abs(x)>=clip || std::abs(y)>=clip);
    }
    return r;
}
inline Power power(const std::complex<float>* samples, std::size_t n, float clip=501.0f/511.0f) {
    if (!n) return {};
    Power r; std::size_t i=0;
#if WINJECT_SIMD_NEON
    float32x4_t sum=vdupq_n_f32(0), rs=vdupq_n_f32(0), is=vdupq_n_f32(0), peak=vdupq_n_f32(0);
    uint32x4_t clipped=vdupq_n_u32(0); const float32x4_t threshold=vdupq_n_f32(clip);
    for (;i+4<=n;i+=4) {
        auto iq=vld2q_f32(reinterpret_cast<const float*>(samples+i));
        auto p=vaddq_f32(vmulq_f32(iq.val[0],iq.val[0]),vmulq_f32(iq.val[1],iq.val[1]));
        sum=vaddq_f32(sum,p); rs=vaddq_f32(rs,iq.val[0]); is=vaddq_f32(is,iq.val[1]); peak=vmaxq_f32(peak,p);
        auto c=vorrq_u32(vcgeq_f32(vabsq_f32(iq.val[0]),threshold),vcgeq_f32(vabsq_f32(iq.val[1]),threshold));
        clipped=vsubq_u32(clipped,c);
    }
    r.sum=vaddvq_f32(sum); r.real_sum=vaddvq_f32(rs); r.imag_sum=vaddvq_f32(is);
    r.peak=vmaxvq_f32(peak); r.clipped=vaddvq_u32(clipped);
#endif
    auto tail=power_scalar(samples+i,n-i,clip);
    r.sum+=tail.sum; r.real_sum+=tail.real_sum; r.imag_sum+=tail.imag_sum;
    r.peak=std::max(r.peak,tail.peak); r.clipped+=tail.clipped; return r;
}
} // namespace winject::simd
