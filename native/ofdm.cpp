#include "ofdm.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numeric>
#if defined(__ARM_NEON)
#include <arm_neon.h>
#endif
namespace winject::ofdm
{
using C = std::complex<double>;
constexpr double PI = 3.14159265358979323846;
static constexpr int ltf[] = {
    1,  1,  -1, -1, 1,  1, -1, 1,  -1, 1, 1,  1,  1,  1, 1,  -1, -1, 1,
    1,  -1, 1,  -1, 1,  1, 1,  1,  0,  1, -1, -1, 1,  1, -1, 1,  -1, 1,
    -1, -1, -1, -1, -1, 1, 1,  -1, -1, 1, -1, 1,  -1, 1, 1,  1,  1};
static constexpr int polarity[] = {
     1,  1,  1,  1, -1, -1, -1,  1, -1, -1, -1, -1,  1,  1, -1,  1, -1, -1,  1,
     1, -1,  1,  1, -1,  1,  1,  1,  1,  1,  1, -1,  1,  1,  1, -1,  1,  1, -1,
    -1,  1,  1,  1, -1,  1, -1, -1, -1,  1, -1,  1, -1, -1,  1, -1, -1,  1,  1,
     1,  1,  1, -1, -1,  1,  1, -1, -1,  1, -1,  1, -1,  1,  1, -1, -1, -1,  1,
     1, -1, -1, -1, -1,  1, -1, -1,  1, -1,  1,  1,  1,  1, -1,  1, -1,  1, -1,
     1, -1, -1, -1, -1, -1,  1, -1,  1,  1, -1,  1, -1,  1,  1,  1, -1, -1,  1,
    -1, -1, -1,  1,  1,  1, -1, -1, -1, -1, -1, -1, -1};
template <size_t N>
static void fft(std::array<C, N>& a, bool inverse = false)
{
    for (int i = 1, j = 0; i < int(N); i++)
    {
        int b = N / 2;
        for (; j & b; b >>= 1)
        {
            j ^= b;
        }
        j ^= b;
        if (i < j)
        {
            std::swap(a[i], a[j]);
        }
    }
    for (int n = 2; n <= int(N); n *= 2)
    {
        C w0 = std::polar(1.0, (inverse ? 2 : -2) * PI / n);
        for (int i = 0; i < int(N); i += n)
        {
            C w = 1;
            for (int j = 0; j < n / 2; j++)
            {
                C u = a[i + j], v = a[i + j + n / 2] * w;
                a[i + j] = u + v;
                a[i + j + n / 2] = u - v;
                w *= w0;
            }
        }
    }
    if (inverse)
    {
        for (auto& v : a)
        {
            v /= N;
        }
    }
}
const char* simd_backend()
{
#if defined(__ARM_NEON)
    return "arm-neon";
#else
    return "scalar";
#endif
}
static void periodic(const std::vector<std::complex<float>>& x, int lag,
                     std::vector<double>& score, std::vector<double>& power)
{
    int m = int(x.size()) - lag, w = 128, n = m - w + 1;
    if (n <= 0)
    {
        return;
    }
    std::vector<double> r(m + 1), im(m + 1), a(m + 1), b(m + 1);
    int i = 0;
    // NEON loads four interleaved complex samples per iteration. Prefix sums
    // are accumulated in double to preserve the detector's threshold behavior.
#if defined(__ARM_NEON)
    for (; i + 4 <= m; i += 4)
    {
        auto u = vld2q_f32(reinterpret_cast<const float*>(x.data() + i));
        auto v = vld2q_f32(reinterpret_cast<const float*>(x.data() + i + lag));
        auto pr = vaddq_f32(vmulq_f32(u.val[0], v.val[0]),
                            vmulq_f32(u.val[1], v.val[1]));
        auto pi = vsubq_f32(vmulq_f32(u.val[1], v.val[0]),
                            vmulq_f32(u.val[0], v.val[1]));
        auto pa = vaddq_f32(vmulq_f32(u.val[0], u.val[0]),
                            vmulq_f32(u.val[1], u.val[1]));
        auto pb = vaddq_f32(vmulq_f32(v.val[0], v.val[0]),
                            vmulq_f32(v.val[1], v.val[1]));
        float rr[4], ii[4], aa[4], bb[4];
        vst1q_f32(rr, pr);
        vst1q_f32(ii, pi);
        vst1q_f32(aa, pa);
        vst1q_f32(bb, pb);
        for (int j = 0; j < 4; j++)
        {
            int k = i + j;
            r[k + 1] = r[k] + rr[j];
            im[k + 1] = im[k] + ii[j];
            a[k + 1] = a[k] + aa[j];
            b[k + 1] = b[k] + bb[j];
        }
    }
#endif
    for (; i < m; i++)
    {
        auto u = x[i], v = x[i + lag];
        auto p = u * std::conj(v);
        r[i + 1] = r[i] + p.real();
        im[i + 1] = im[i] + p.imag();
        a[i + 1] = a[i] + std::norm(u);
        b[i + 1] = b[i] + std::norm(v);
    }
    score.resize(n);
    power.resize(n);
    for (i = 0; i < n; i++)
    {
        double pr = r[i + w] - r[i], pi = im[i + w] - im[i],
               ea = a[i + w] - a[i], eb = b[i + w] - b[i];
        score[i] = (pr * pr + pi * pi) / (ea * eb + 1e-15);
        power[i] = ea / w;
    }
}
static void detect_signal(const std::vector<std::complex<float>>& x, Candidate& c);
static void training(const std::vector<std::complex<float>>& x, Candidate& c)
{
    if (c.long_score < .90 || c.long_start_sample + 256 > int(x.size()) ||
        c.start_sample + 288 > int(x.size()))
    {
        return;
    }
    C corr = 0;
    for (int i = 0; i < 256; i++)
    {
        corr +=
            std::conj(C(x[c.start_sample + i])) * C(x[c.start_sample + i + 32]);
    }
    double w = std::arg(corr) / 32;
    std::array<C, 128> a, b;
    int clipped = 0;
    for (int i = 0; i < 256; i++)
    {
        auto z = x[c.long_start_sample + i];
        if (std::abs(z.real()) >= .98 || std::abs(z.imag()) >= .98)
        {
            clipped++;
        }
        C q = C(z) * std::polar(1., -w * i);
        if (i < 128)
        {
            a[i] = q;
        }
        else
        {
            b[i - 128] = q;
        }
    }
    c.training_clipped_fraction = float(clipped) / 256;
    if (c.training_clipped_fraction > .005)
    {
        return;
    }
    fft(a);
    fft(b);
    corr = 0;
    for (int k = -26; k <= 26; k++)
    {
        if (k)
        {
            int bin = (k + 128) % 128;
            corr += std::conj(a[bin]) * b[bin];
        }
    }
    C rotation = std::polar(1., -std::arg(corr));
    std::array<C, 52> h, v;
    std::array<double, 52> energy;
    int j = 0;
    for (int k = -26; k <= 26; k++)
    {
        if (k)
        {
            int bin = (k + 128) % 128;
            h[j] = a[bin] * double(ltf[k + 26]);
            v[j] = b[bin] * rotation;
            energy[j] = std::norm(h[j]);
            j++;
        }
    }
    auto sorted = energy;
    std::sort(sorted.begin(), sorted.end());
    double median = (sorted[25] + sorted[26]) / 2;
    if (!std::isfinite(median) || median < 1e-10)
    {
        return;
    }
    C co = 0;
    double e1 = 0, e2 = 0;
    for (int i = 0; i < 51; i++)
    {
        if (i == 25)
        {
            continue;
        }
        co += std::conj(h[i]) * h[i + 1];
        e1 += std::norm(h[i]);
        e2 += std::norm(h[i + 1]);
    }
    double coherence = std::abs(co) / std::max(std::sqrt(e1 * e2), 1e-15);
    if (!std::isfinite(coherence) || coherence < .70)
    {
        return;
    }
    double err = 0;
    int count = 0;
    j = 0;
    for (int k = -26; k <= 26; k++)
    {
        if (k)
        {
            if (energy[j] > median * .04)
            {
                C z = v[j] / h[j];
                c.training_constellation.emplace_back(z);
                err += std::norm(z - double(ltf[k + 26]));
                count++;
            }
            j++;
        }
    }
    if (count < 40)
    {
        c.training_constellation.clear();
        return;
    }
    double evm = 100 * std::sqrt(err / count);
    if (!std::isfinite(evm) || evm > 100)
    {
        c.training_constellation.clear();
        return;
    }
    double total = std::accumulate(energy.begin(), energy.end(), 0.) / 52;
    for (auto z : h)
    {
        c.channel_estimate.emplace_back(z / std::sqrt(total));
    }
    c.training_valid = true;
    c.training_evm_percent = evm;
    c.channel_coherence = coherence;
    c.carrier_offset_hz = w * 40000000 / (2 * PI);
}
std::vector<Candidate> detect(const std::vector<std::complex<float>>& x,
                              int rate)
{
    if (rate != 40000000 || x.size() < 1024)
    {
        return {};
    }
    for (auto v : x)
    {
        if (!std::isfinite(v.real()) || !std::isfinite(v.imag()))
        {
            return {};
        }
    }
    std::vector<double> s, p, l, unused;
    periodic(x, 32, s, p);
    periodic(x, 128, l, unused);
    std::vector<Candidate> out;
    int last = -1024;
    for (int i = 0; i < int(s.size());)
    {
        if (!(s[i] > .82 && p[i] > 16.0 / (127.75 * 127.75)))
        {
            i++;
            continue;
        }
        int begin = i;
        while (i < int(s.size()) && s[i] > .82 &&
               p[i] > 16.0 / (127.75 * 127.75))
        {
            i++;
        }
        if (i - begin < 24)
        {
            continue;
        }
        int start =
            int(std::max_element(s.begin() + begin, s.begin() + i) - s.begin());
        int lo = start + 80, hi = std::min(start + 600, int(l.size()));
        if (lo >= hi)
        {
            continue;
        }
        int ls =
            int(std::max_element(l.begin() + lo, l.begin() + hi) - l.begin());
        if (l[ls] < .82 || start - last < 256)
        {
            continue;
        }
        out.push_back({start, ls, float(s[start]), float(l[ls])});
        training(x, out.back());
        if (out.back().short_score >= .90 && out.back().long_score >= .90)
            detect_signal(x, out.back());
        last = start;
    }
    return out;
}
static std::vector<uint8_t> viterbi(const std::vector<uint8_t>& in, bool zero)
{
    size_t n = in.size() / 2;
    std::vector<uint8_t> history(n * 64), out(n);
    std::array<int, 64> m{}, v{};
    m.fill(1000000);
    m[0] = 0;
    for (size_t i = 0; i < n; i++)
    {
        for (int s = 0; s < 64; s++)
        {
            int p = s >> 1, q = p | 32, bit = s & 1, r = (p << 1) | bit,
                t = (q << 1) | bit;
            int x =
                m[p] +
                (in[i * 2] != 2 && __builtin_parity(r & 0155) != in[i * 2]) +
                (in[i * 2 + 1] != 2 &&
                 __builtin_parity(r & 0117) != in[i * 2 + 1]);
            int y =
                m[q] +
                (in[i * 2] != 2 && __builtin_parity(t & 0155) != in[i * 2]) +
                (in[i * 2 + 1] != 2 &&
                 __builtin_parity(t & 0117) != in[i * 2 + 1]);
            history[i * 64 + s] = y < x;
            v[s] = y < x ? y : x;
        }
        m = v;
    }
    int s = zero ? 0 : int(std::min_element(m.begin(), m.end()) - m.begin());
    for (size_t i = n; i > 0; i--)
    {
        out[i - 1] = s & 1;
        s = (s >> 1) | (history[(i - 1) * 64 + s] ? 32 : 0);
    }
    return out;
}
static bool descramble(std::vector<uint8_t>& b)
{
    int seed = 0, matches = 0;
    for (int s = 1; s < 128; s++)
    {
        int q = s;
        bool ok = true;
        for (int i = 0; i < 16; i++)
        {
            int f = ((q >> 6) ^ (q >> 3)) & 1;
            if (b[i] != f)
            {
                ok = false;
                break;
            }
            q = ((q << 1) | f) & 127;
        }
        if (ok)
        {
            seed = s;
            matches++;
        }
    }
    if (matches != 1)
    {
        return false;
    }
    int q = seed;
    for (auto& v : b)
    {
        int f = ((q >> 6) ^ (q >> 3)) & 1;
        v ^= f;
        q = ((q << 1) | f) & 127;
    }
    return true;
}
static uint32_t crc32(const uint8_t* p, size_t n)
{
    uint32_t c = ~0u;
    for (size_t i = 0; i < n; i++)
    {
        c ^= p[i];
        for (int j = 0; j < 8; j++)
        {
            c = (c >> 1) ^ (0xedb88320u & -(c & 1));
        }
    }
    return ~c;
}
struct Rate
{
    int rate, bpsc, dbps;
    std::vector<int> punct;
};
static std::optional<Rate> getrate(const std::vector<uint8_t>& b)
{
    int key = b[0] | b[1] << 1 | b[2] << 2 | b[3] << 3;
    switch (key)
    {
        case 11:
            return Rate{6, 1, 24, {1, 1}};
        case 15:
            return Rate{9, 1, 36, {1, 1, 1, 0, 0, 1}};
        case 10:
            return Rate{12, 2, 48, {1, 1}};
        case 14:
            return Rate{18, 2, 72, {1, 1, 1, 0, 0, 1}};
        case 9:
            return Rate{24, 4, 96, {1, 1}};
        case 13:
            return Rate{36, 4, 144, {1, 1, 1, 0, 0, 1}};
        case 8:
            return Rate{48, 6, 192, {1, 1, 1, 0}};
        case 12:
            return Rate{54, 6, 216, {1, 1, 1, 0, 0, 1}};
        default:
            return {};
    }
}
struct PreparedSignal
{
    struct Phase {
        std::vector<C> z;
        std::array<C,64> h;
        int first, phase, len;
        double norm;
        Rate info;
    };
    std::vector<Phase> phases;
};
// L-LTF SNR: the two LTF symbols carry the same samples, so after the fine CFO
// correction their difference is noise only (E|y1-y2|^2 = 2 sigma^2 per subcarrier)
// and their mean holds the signal plus sigma^2 / 2. Summed over the 52 occupied
// subcarriers this is the average-power SNR of one symbol. NaN when not measurable.
static float ltf_snr(const std::vector<C>& z, int first)
{
    if (first < 0 || first + 128 > int(z.size())) return std::numeric_limits<float>::quiet_NaN();
    std::array<C, 64> l1, l2;
    for (int i = 0; i < 64; i++)
    {
        l1[i] = z[first + i];
        l2[i] = z[first + 64 + i];
    }
    fft(l1);
    fft(l2);
    double noise = 0, signal = 0;
    for (int k = -26; k <= 26; k++)
    {
        if (!k) continue;
        const int b = (k + 64) % 64;
        const double nk = std::norm(l1[b] - l2[b]) / 2;
        noise += nk;
        signal += std::norm((l1[b] + l2[b]) / 2.0) - nk / 2;
    }
    return noise > 0 && signal > 0 ? float(10 * std::log10(signal / noise))
                                   : std::numeric_limits<float>::quiet_NaN();
}

static std::shared_ptr<PreparedSignal> prepare(const std::vector<std::complex<float>>& raw,
                                               const Candidate& c, int rate)
{
    if (rate != 40000000 || c.start_sample < 0 ||
        c.start_sample + 288 > int(raw.size()) || c.long_start_sample < 0)
    {
        return {};
    }
    auto prepared = std::make_shared<PreparedSignal>();
    // The receiver's DC offset (often well above the noise floor) must go before the
    // CFO de-rotation: rotated, it becomes a tone whose phase differs between the two
    // L-LTF symbols, so it reads as noise on the subcarriers next to DC and biases the
    // coarse CFO toward zero. Legacy OFDM carries nothing at DC, so the capture mean is
    // the offset.
    C dc = 0;
    for (auto v : raw)
    {
        dc += C(v);
    }
    dc /= double(raw.size());
    C corr = 0;
    for (int i = 0; i < 224; i++)
    {
        corr += std::conj(C(raw[c.start_sample + i]) - dc) *
                (C(raw[c.start_sample + i + 32]) - dc);
    }
    double coarse = std::arg(corr) / 32;
    std::vector<C> x(raw.size());
    for (size_t i = 0; i < raw.size(); i++)
    {
        x[i] = (C(raw[i]) - dc) * std::polar(1.0, -coarse * i);
    }
    std::array<C, 64> lt{};
    for (int k = -26; k <= 26; k++)
    {
        if (k)
        {
            lt[(k + 64) % 64] = ltf[k + 26];
        }
    }
    fft(lt, true);
    double lt_energy = 0;
    for (auto q : lt)
    {
        lt_energy += std::norm(q);
    }
    for (int phase = 0; phase < 2; phase++)
    {
        std::vector<C> z;
        for (size_t i = phase; i < x.size(); i += 2)
        {
            z.push_back(x[i]);
        }
        int approx = std::max(0, (c.long_start_sample - phase) / 2),
            lo = std::max(0, approx - 96),
            hi = std::min(int(z.size()) - 128, approx + 96);
        if (hi <= lo)
        {
            continue;
        }
        std::vector<C> cc(hi - lo + 64);
        for (int j = 0; j < int(cc.size()); j++)
        {
            for (int i = 0; i < 64; i++)
            {
                cc[j] += z[lo + j + i] * std::conj(lt[i]);
            }
        }
        int best = 0;
        double score = -1;
        for (int j = 0; j < hi - lo; j++)
        {
            double q = std::abs(cc[j]) + std::abs(cc[j + 64]);
            if (q > score)
            {
                best = j;
                score = q;
            }
        }
        int first = lo + best;
        double energy = 0;
        for (int i = 0; i < 64; i++)
        {
            energy += std::norm(z[first + i]);
        }
        double norm =
            std::abs(cc[best]) / (std::sqrt(energy * lt_energy) + 1e-15);
        if (norm < .2 || first + 208 > int(z.size()))
        {
            continue;
        }
        corr = 0;
        for (int i = 0; i < 64; i++)
        {
            corr += std::conj(z[first + i]) * z[first + 64 + i];
        }
        double fine = std::arg(corr) / 64;
        for (int i = 0; i < int(z.size()); i++)
        {
            z[i] *= std::polar(1.0, -fine * (i - first));
        }
        std::array<C, 64> y1, y2, h;
        for (int i = 0; i < 64; i++)
        {
            y1[i] = z[first + i];
            y2[i] = z[first + 64 + i];
        }
        fft(y1);
        fft(y2);
        for (int i = 0; i < 64; i++)
        {
            h[i] = (y1[i] + y2[i]) / 2.0;
        }
        bool bad = false;
        for (int k = -26; k <= 26; k++)
        {
            if (k)
            {
                int b = (k + 64) % 64;
                h[b] *= double(ltf[k + 26]);
                if (std::abs(h[b]) < 1e-12)
                {
                    bad = true;
                }
            }
        }
        if (bad)
        {
            continue;
        }
        auto symbol = [&](int start, int pol, std::array<C, 48>& out) -> bool
        {
            if (start + 80 > int(z.size()))
            {
                return false;
            }
            std::array<C, 64> a;
            for (int i = 0; i < 64; i++)
            {
                a[i] = z[start + 16 + i];
            }
            fft(a);
            for (int i = 0; i < 64; i++)
            {
                a[i] /= std::abs(h[i]) > 1e-12 ? h[i] : C(1);
            }
            C p = 0;
            for (int k : {-21, -7, 7, 21})
            {
                p += a[(k + 64) % 64] *
                     double(polarity[pol % 127] * (k == 21 ? -1 : 1));
            }
            if (std::abs(p) < 1e-12)
            {
                return false;
            }
            C rot = std::polar(1.0, -std::arg(p));
            int j = 0;
            for (int k = -26; k <= 26; k++)
            {
                if (k && k != -21 && k != -7 && k != 7 && k != 21)
                {
                    out[j++] = a[(k + 64) % 64] * rot;
                }
            }
            return true;
        };
        std::array<C, 48> sig;
        if (!symbol(first + 128, 0, sig))
        {
            continue;
        }
        std::vector<uint8_t> sb(48);
        for (int k = 0; k < 48; k++)
        {
            sb[k] = sig[3 * (k % 16) + k / 16].real() > 0;
        }
        auto dec = viterbi(sb, true);
        auto info = getrate(dec);
        if (!info || dec[4])
        {
            continue;
        }
        int parity = 0;
        for (int i = 0; i < 18; i++)
        {
            parity += dec[i];
        }
        if (parity % 2 || std::any_of(dec.begin() + 18, dec.begin() + 24,
                                      [](auto b)
                                      {
                                          return b != 0;
                                      }))
        {
            continue;
        }
        int len = 0;
        for (int i = 0; i < 12; i++)
        {
            len |= dec[5 + i] << i;
        }
        if (len < 1 || len > 4095)
        {
            continue;
        }
        prepared->phases.push_back({std::move(z), h, first, phase, len, norm, *info});
    }
    return prepared;
}

static void detect_signal(const std::vector<std::complex<float>>& raw, Candidate& c)
{
    auto prepared = prepare(raw, c, 40000000);
    if (!prepared || prepared->phases.empty()) return;
    const auto& p = prepared->phases.front();
    c.signal_valid = true;
    c.signal_rate_mbps = p.info.rate;
    c.signal_length_bytes = p.len;
    c.signal_modulation = p.info.bpsc == 1 ? "BPSK" : p.info.bpsc == 2 ? "QPSK" :
                          p.info.bpsc == 4 ? "16-QAM" : "64-QAM";
    c.prepared = std::move(prepared);
}

std::optional<Decoded> decode(const std::vector<std::complex<float>>& raw,
                              const Candidate& c, int rate)
{
    if (rate != 40000000) return {};
    auto prepared = c.prepared ? c.prepared : prepare(raw, c, rate);
    if (!prepared) return {};
    for (const auto& state : prepared->phases)
    {
        const auto& z = state.z;
        const auto& h = state.h;
        int first = state.first, phase = state.phase, len = state.len;
        double norm = state.norm;
        const Rate* info = &state.info;
        if (len < 14) continue;
        bool bad = false;
        auto symbol = [&](int start, int pol, std::array<C, 48>& out, std::array<C, 48>& before_pilot) -> bool
        {
            if (start + 80 > int(z.size()))
            {
                return false;
            }
            std::array<C, 64> a;
            for (int i = 0; i < 64; i++)
            {
                a[i] = z[start + 16 + i];
            }
            fft(a);
            for (int i = 0; i < 64; i++)
            {
                a[i] /= std::abs(h[i]) > 1e-12 ? h[i] : C(1);
            }
            C p = 0;
            for (int k : {-21, -7, 7, 21})
            {
                p += a[(k + 64) % 64] *
                     double(polarity[pol % 127] * (k == 21 ? -1 : 1));
            }
            if (std::abs(p) < 1e-12)
            {
                return false;
            }
            C rot = std::polar(1.0, -std::arg(p));
            int j = 0;
            for (int k = -26; k <= 26; k++)
            {
                if (k && k != -21 && k != -7 && k != 7 && k != 21)
                {
                    before_pilot[j] = a[(k + 64) % 64];
                    out[j] = before_pilot[j] * rot;
                    j++;
                }
            }
            return true;
        };
        int n = (16 + len * 8 + 6 + info->dbps - 1) / info->dbps;
        if (first + 128 + 80 * (n + 1) > int(z.size()))
        {
            continue;
        }
        int ncbps = 48 * info->bpsc, spacing = std::max(info->bpsc / 2, 1);
        std::vector<uint8_t> coded;
        std::vector<C> points, ideals, raw_points;
        static const int map1[] = {-1, 1}, map2[] = {-3, -1, 3, 1},
                         map3[] = {-7, -5, -1, -3, 7, 5, 1, 3};
        int half = info->bpsc / 2;
        const int* map = half == 1 ? map1 : half == 2 ? map2 : map3;
        double scale = info->bpsc == 2   ? std::sqrt(2.)
                       : info->bpsc == 4 ? std::sqrt(10.)
                                         : std::sqrt(42.);
        for (int si = 0; si < n; si++)
        {
            std::array<C, 48> q, before_pilot;
            if (!symbol(first + 208 + si * 80, si + 1, q, before_pilot))
            {
                bad = true;
                break;
            }
            std::vector<uint8_t> inter(ncbps);
            for (int j = 0; j < 48; j++)
            {
                C ideal;
                int index = 0;
                if (info->bpsc == 1)
                {
                    index = q[j].real() > 0;
                    ideal = C(index ? 1 : -1, 0);
                }
                else
                {
                    int ai = 0, aq = 0;
                    double di = 1e99, dq = 1e99;
                    for (int m = 0; m < (1 << half); m++)
                    {
                        double v = map[m] / scale,
                               d = std::abs(q[j].real() - v);
                        if (d < di)
                        {
                            di = d;
                            ai = m;
                        }
                        d = std::abs(q[j].imag() - v);
                        if (d < dq)
                        {
                            dq = d;
                            aq = m;
                        }
                    }
                    index = (ai << half) | aq;
                    ideal = C(map[ai] / scale, map[aq] / scale);
                }
                for (int b = 0; b < info->bpsc; b++)
                {
                    inter[j * info->bpsc + b] =
                        (index >> (info->bpsc - 1 - b)) & 1;
                }
                points.push_back(q[j]);
                if (raw_points.size() < 1024) raw_points.push_back(before_pilot[j]);
                ideals.push_back(ideal);
            }
            for (int k = 0; k < ncbps; k++)
            {
                int i = (ncbps / 16) * (k % 16) + k / 16,
                    j = spacing * (i / spacing) +
                        (i + ncbps - (16 * i) / ncbps) % spacing;
                coded.push_back(inter[j]);
            }
        }
        if (bad)
        {
            continue;
        }
        std::vector<uint8_t> mother;
        size_t j = 0;
        while (j < coded.size())
        {
            for (int v : info->punct)
            {
                mother.push_back(v ? coded[j++] : 2);
            }
        }
        auto bits = viterbi(mother, false);
        if (bits.size() < size_t(16 + len * 8) || !descramble(bits) ||
            std::any_of(bits.begin(), bits.begin() + 16,
                        [](auto b)
                        {
                            return b != 0;
                        }))
        {
            continue;
        }
        std::vector<uint8_t> bytes(len);
        for (int i = 0; i < len; i++)
        {
            for (int b = 0; b < 8; b++)
            {
                bytes[i] |= bits[16 + i * 8 + b] << b;
            }
        }
        uint32_t f = uint32_t(bytes[len - 4]) | uint32_t(bytes[len - 3]) << 8 |
                     uint32_t(bytes[len - 2]) << 16 |
                     uint32_t(bytes[len - 1]) << 24;
        if (crc32(bytes.data(), len - 4) != f)
        {
            continue;
        }
        Decoded out;
        out.mpdu.assign(bytes.begin(), bytes.end() - 4);
        out.rate_mbps = info->rate;
        out.modulation = info->bpsc == 1   ? "BPSK"
                         : info->bpsc == 2 ? "QPSK"
                         : info->bpsc == 4 ? "16-QAM"
                                           : "64-QAM";
        out.ltf_start_sample = first * 2 + phase;
        out.frame_end_sample = (first + 128 + 80 * (n + 1)) * 2 + phase;
        out.ltf_match = norm;
        out.ltf_snr_db = ltf_snr(z, first);
        out.raw_data_iq.assign(raw_points.begin(), raw_points.end());
        // L-LTF has no pilots. Show both FFT symbols after channel equalization,
        // using the same CFO-corrected samples and channel as the data symbols.
        for (int symbol_index = 0; symbol_index < 2; symbol_index++)
        {
            std::array<C, 64> training;
            for (int i = 0; i < 64; i++) training[i] = z[first + symbol_index * 64 + i];
            fft(training);
            for (int k = -26; k <= 26; k++) if (k)
            {
                int bin = (k + 64) % 64;
                out.raw_training_iq.emplace_back(training[bin] / h[bin]);
            }
        }
        double err = 0, pow = 0;
        for (size_t i = 0; i < points.size(); i++)
        {
            err += std::norm(points[i] - ideals[i]);
            pow += std::norm(ideals[i]);
            if (i < 1024)
            {
                out.constellation.emplace_back(points[i]);
            }
        }
        out.evm_percent = 100 * std::sqrt(err / pow);
        int begin = first * 2 + phase,
            end = std::min(int(raw.size()),
                           (first + 128 + 80 * (n + 1)) * 2 + phase),
            clipped = 0;
        for (int i = begin; i < end; i++)
        {
            if (std::abs(raw[i].real()) >= .98 ||
                std::abs(raw[i].imag()) >= .98)
            {
                clipped++;
            }
        }
        out.clipped_fraction = double(clipped) / (end - begin);
        out.evm_valid = out.clipped_fraction <= .005;
        return out;
    }
    return {};
}
float ltf_snr_db(const Candidate& c)
{
    if (!c.prepared || c.prepared->phases.empty()) return std::numeric_limits<float>::quiet_NaN();
    const auto& p = c.prepared->phases.front();
    return ltf_snr(p.z, p.first);
}
}  // namespace winject::ofdm
