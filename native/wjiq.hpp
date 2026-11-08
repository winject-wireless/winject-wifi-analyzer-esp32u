#pragma once
#include <complex>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <vector>

namespace winject {
struct Capture {
    uint32_t id{}, count{}, timestamp_us{};
    uint64_t frequency_hz{};
    uint16_t job{}, step{}, steps{};
    uint8_t flags{}, bits{}, rate_code{}, gain{}, bandwidth{};
    std::vector<uint8_t> payload;
    int rate() const { return rate_code == 0 ? 80000000 : rate_code == 1 ? 40000000 : 16000000; }
    std::vector<std::complex<float>> samples() const;
};
class Assembler {
public:
    std::optional<Capture> add(const uint8_t* data, size_t size);
    uint64_t completed=0, lost=0, errors=0;
private:
    struct Partial { Capture cap; std::vector<uint8_t> bytes, seen; size_t received=0; };
    std::map<uint32_t, Partial> partial_;
    std::deque<uint32_t> order_;
};
}
