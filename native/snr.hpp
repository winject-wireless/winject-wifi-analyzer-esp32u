#pragma once
// In-band noise floor and SNR of one frame inside a capture.
//
// Both powers are measured in the band a legacy OFDM frame occupies (subcarriers
// -26..26 = +-8.125 MHz, DC excluded) with 64-sample FFT blocks at 40 MS/s, so the
// out-of-band noise of the 40 MHz capture does not dilute the SNR.
//
// Noise floor: the blocks outside the frame (plus a guard) are sorted by in-band
// power and the 25th percentile is taken, so other traffic in up to ~3/4 of the
// quiet capture does not lift it. For pure noise each block is chi-square with 52
// degrees of freedom; the percentile is divided by that quantile (Wilson-Hilferty)
// so the estimate is unbiased.
//
// Signal: mean in-band power over the frame minus the noise floor.
#include <algorithm>
#include <cmath>
#include <complex>
#include <vector>

namespace winject::snr {

struct Estimate
{
    double frame_inband = NAN;  // mean in-band power over the frame, signal + noise
    double noise = NAN;         // in-band noise floor
    double snr_db = NAN;        // NaN when the frame does not rise above the floor
    int noise_blocks = 0;
};

inline constexpr int k_block = 64;     // 625 kHz bins at 40 MS/s
inline constexpr int k_half_band = 13; // bins 1..13 and -13..-1: +-8.125 MHz
inline constexpr int k_guard = 200;    // samples kept clear around the frame
inline constexpr int k_min_noise_blocks = 8;

inline void fft64(std::complex<double>* a)
{
    const int n = k_block;
    for (int i = 1, j = 0; i < n; ++i)
    {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (int len = 2; len <= n; len <<= 1)
    {
        const std::complex<double> wl = std::polar(1.0, -2 * M_PI / len);
        for (int i = 0; i < n; i += len)
        {
            std::complex<double> w(1, 0);
            for (int j = 0; j < len / 2; ++j, w *= wl)
            {
                const auto u = a[i + j], v = a[i + j + len / 2] * w;
                a[i + j] = u + v;
                a[i + j + len / 2] = u - v;
            }
        }
    }
}

// In-band share of the mean sample power of one block (Parseval: divide by N^2).
inline double inband_power(const std::complex<float>* x, std::complex<double> dc)
{
    std::complex<double> v[k_block];
    for (int i = 0; i < k_block; ++i) v[i] = std::complex<double>(x[i]) - dc;
    fft64(v);
    double p = 0;
    for (int k = 1; k <= k_half_band; ++k) p += std::norm(v[k]) + std::norm(v[k_block - k]);
    return p / (double(k_block) * k_block);
}

// q-quantile of chi-square(k) divided by its mean k (Wilson-Hilferty).
inline double chi2_quantile_over_mean(double k, double z)
{
    const double a = 2.0 / (9.0 * k);
    const double c = 1.0 - a + z * std::sqrt(a);
    return c * c * c;
}

// Noise floor of a whole capture (no frame excluded): the same corrected 25th percentile.
// A busy capture reads high; callers keep a low percentile over recent captures.
inline double capture_floor(const std::vector<std::complex<float>>& x)
{
    const int n = int(x.size());
    if (n < k_block * k_min_noise_blocks) return NAN;
    std::complex<double> dc{};
    for (auto z : x) dc += std::complex<double>(z);
    dc /= double(n);
    std::vector<double> blocks;
    for (int b = 0; b + k_block <= n; b += k_block) blocks.push_back(inband_power(&x[b], dc));
    const size_t q = blocks.size() / 4;
    std::nth_element(blocks.begin(), blocks.begin() + q, blocks.end());
    return blocks[q] / chi2_quantile_over_mean(4.0 * k_half_band, -0.6745);
}

inline Estimate estimate(const std::vector<std::complex<float>>& x, int start, int end)
{
    Estimate e;
    const int n = int(x.size());
    start = std::max(0, start);
    end = std::min(n, end);
    if (end - start < k_block) return e;
    std::complex<double> dc{};
    for (auto z : x) dc += std::complex<double>(z);
    dc /= double(n);

    double sum = 0;
    int count = 0;
    for (int b = start; b + k_block <= end; b += k_block, ++count) sum += inband_power(&x[b], dc);
    e.frame_inband = sum / count;

    std::vector<double> quiet;
    for (int b = 0; b + k_block <= start - k_guard; b += k_block) quiet.push_back(inband_power(&x[b], dc));
    for (int b = end + k_guard; b + k_block <= n; b += k_block) quiet.push_back(inband_power(&x[b], dc));
    e.noise_blocks = int(quiet.size());
    if (e.noise_blocks < k_min_noise_blocks) return e;
    const size_t q = quiet.size() / 4;
    std::nth_element(quiet.begin(), quiet.begin() + q, quiet.end());
    e.noise = quiet[q] / chi2_quantile_over_mean(4.0 * k_half_band, -0.6745);
    if (e.frame_inband > e.noise) e.snr_db = 10 * std::log10((e.frame_inband - e.noise) / e.noise);
    return e;
}

} // namespace winject::snr
