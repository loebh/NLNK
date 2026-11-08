#pragma once

#include "vqf/vqf_common.hpp"

#include <cstdint>

namespace vqf::detail {

struct FingerprintProbeResult {
    size_type found_slot;
    size_type insertion_slot;
};

bool avx2_scan_available() noexcept;

FingerprintProbeResult probe_fingerprint_avx2(
    const std::uint8_t* fingerprints, const std::uint8_t* slot_states,
    size_type capacity, size_type start, std::uint64_t low,
    std::uint64_t high, unsigned fingerprint_bits) noexcept;

}  // namespace vqf::detail
