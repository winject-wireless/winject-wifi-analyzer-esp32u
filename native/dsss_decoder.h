#pragma once
#include <complex>
#include <cstddef>
#include <cstdint>
#include <vector>
namespace winject {
struct DsssFrame {
    std::vector<std::uint8_t> bytes; // Includes four-byte FCS.
    std::uint32_t rate_bps;
    std::size_t start_sample;          // Approximate; in source-rate samples.
    bool short_preamble;
};
// Experimental burst-local receiver at 20 or 40 MS/s. No state is retained
// across captures; only complete, FCS-valid frames are returned.
std::vector<DsssFrame> decode_dsss(const std::complex<float>* iq, std::size_t count, unsigned sample_rate_hz);
}
