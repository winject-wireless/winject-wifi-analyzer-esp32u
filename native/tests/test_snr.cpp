// SNR / noise floor estimator: known band-limited signal in white noise.
#include "snr.hpp"
#include <cstdio>
#include <random>

static int failures = 0;
static void check(bool ok, const char* what, double got, double want)
{
    if (!ok) { std::printf("FAIL %s: got %.3f want %.3f\n", what, got, want); ++failures; }
}

// OFDM-like signal: random QPSK on subcarriers -26..26 (no DC) at 20 MHz spacing,
// i.e. every other 625 kHz bin of a 64-point FFT at 40 MS/s, unit mean power.
static std::vector<std::complex<float>> frame_signal(int n, std::mt19937& rng)
{
    std::vector<std::complex<float>> out(n);
    std::uniform_int_distribution<int> bit(0, 1);
    for (int b = 0; b < n; b += 128)
    {
        std::complex<double> v[128]{};
        for (int k = -26; k <= 26; ++k)
            if (k) v[(k + 128) % 128] = {bit(rng) ? 1.0 : -1.0, bit(rng) ? 1.0 : -1.0};
        // inverse DFT of 128 points
        for (int t = 0; t < 128 && b + t < n; ++t)
        {
            std::complex<double> s{};
            for (int k = 0; k < 128; ++k) s += v[k] * std::polar(1.0, 2 * M_PI * k * t / 128);
            out[b + t] = std::complex<float>(s / std::sqrt(2.0 * 52 * 128 * 128 / 128.0));
        }
    }
    return out;
}

static double db(double p) { return 10 * std::log10(p); }

int main()
{
    std::mt19937 rng(7);
    const int n = 16380, start = 4000, end = 9000;
    for (double snr_target : {5.0, 15.0, 30.0})
    {
        for (bool busy : {false, true})
        {
            const double noise_total = 1e-3;  // white over the whole 40 MHz
            std::normal_distribution<float> g(0, std::sqrt(noise_total / 2));
            std::vector<std::complex<float>> x(n);
            for (auto& z : x) z = {g(rng), g(rng)};
            for (auto& z : x) z += std::complex<float>(0.01f, -0.02f);  // DC offset
            const double noise_inband = noise_total * 26.0 / 64.0;
            const double sig = noise_inband * std::pow(10, snr_target / 10);
            auto s = frame_signal(end - start, rng);
            double sp = 0;
            for (auto z : s) sp += std::norm(z);
            sp /= s.size();
            const float scale = float(std::sqrt(sig / sp));
            for (int i = start; i < end; ++i) x[i] += s[i - start] * scale;
            if (busy)  // another station on air for 2.5 us twice outside the frame
                for (int off : {1000, 12000})
                    for (int i = 0; i < 100; ++i) x[off + i] += s[i] * scale * 3.0f;
            const auto e = winject::snr::estimate(x, start, end);
            check(std::fabs(db(e.noise) - db(noise_inband)) < 0.5, "noise floor dB", db(e.noise), db(noise_inband));
            check(std::fabs(e.snr_db - snr_target) < 0.6, "snr dB", e.snr_db, snr_target);
        }
    }
    // Frame filling the capture: no quiet blocks, no estimate.
    std::vector<std::complex<float>> full(2000, {0.1f, 0.f});
    const auto e = winject::snr::estimate(full, 100, 1900);
    check(std::isnan(e.noise) && std::isnan(e.snr_db), "no noise estimate without quiet blocks", e.noise, NAN);
    if (!failures) std::printf("snr ok\n");
    return failures ? 1 : 0;
}
