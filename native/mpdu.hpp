#pragma once
#include <cstdint>
#include <optional>
#include <vector>
#include <nlohmann/json.hpp>
namespace winject {
// Only call parse_verified_mpdu after the PHY has verified the FCS.
std::optional<nlohmann::json> parse_verified_mpdu(const std::vector<uint8_t>& mpdu);
std::optional<nlohmann::json> parse_mpdu_with_fcs(const std::vector<uint8_t>& frame);
}
