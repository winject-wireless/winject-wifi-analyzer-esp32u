#pragma once
#include <complex>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <vector>
namespace winject::ofdm
{
struct PreparedSignal;
struct Candidate
{
    int start_sample = 0, long_start_sample = 0;
    float short_score = 0, long_score = 0;
    bool training_valid = false;
    float training_evm_percent = 0, channel_coherence = 0;
    float carrier_offset_hz = 0, training_clipped_fraction = 0;
    std::vector<std::complex<float>> training_constellation;
    std::vector<std::complex<float>>
        channel_estimate;  // carriers -26..-1,+1..+26
    // Legacy L-SIGNAL is parity/tail validated, but the MPDU is not yet FCS verified.
    bool signal_valid = false;
    int signal_rate_mbps = 0, signal_length_bytes = 0;
    std::string signal_modulation;
    std::shared_ptr<const PreparedSignal> prepared;
};
struct Decoded
{
    std::vector<uint8_t> mpdu;
    int rate_mbps = 0;
    std::string modulation;
    std::vector<std::complex<float>> constellation, raw_data_iq, raw_training_iq;
    int ltf_start_sample = 0, frame_end_sample = 0;
    float evm_percent = 0, clipped_fraction = 0, ltf_match = 0;
    bool evm_valid = false;
    // SNR from the two identical L-LTF symbols (NaN when not measurable).
    float ltf_snr_db = std::numeric_limits<float>::quiet_NaN();
};
std::vector<Candidate> detect(const std::vector<std::complex<float>>&,
                              int sample_rate_hz);
std::optional<Decoded> decode(const std::vector<std::complex<float>>&,
                              const Candidate&, int sample_rate_hz);
// L-LTF SNR of a candidate whose SIGNAL decoded (it does not need the rest of the
// frame, so it also works for frames cut off by the end of the capture).
float ltf_snr_db(const Candidate&);
const char* simd_backend();
}  // namespace winject::ofdm
