#pragma once

#include "vqf_common.hpp"

#include <cstdint>
#include <unordered_set>
#include <vector>

namespace vqf {

enum class DeletionMode {
    fast,
    exact
};

enum class FingerprintScanMode {
    automatic,
    scalar
};

struct FingerprintFilterConfig {
    size_type capacity;
    unsigned fingerprint_bits = 16;
    DeletionMode deletion_mode = DeletionMode::fast;
    FingerprintScanMode scan_mode = FingerprintScanMode::scalar;
};

class FingerprintFilter {
public:
    explicit FingerprintFilter(const FingerprintFilterConfig& config);

    bool insert(value_type key);
    bool query(value_type key) const noexcept;
    bool erase_fast(value_type key);
    bool erase_exact(value_type key);

    size_type capacity() const noexcept;
    size_type size() const noexcept;
    DeletionMode deletion_mode() const noexcept;
    bool uses_avx2() const noexcept;

private:
    static constexpr std::uint8_t empty_slot = 0;
    static constexpr std::uint8_t occupied_slot = 1;
    static constexpr std::uint8_t deleted_slot = 2;

    value_type fingerprint(value_type key) const noexcept;
    size_type find_fingerprint(value_type value) const noexcept;
    bool add_fingerprint(value_type value) noexcept;
    void remove_fingerprint(size_type slot) noexcept;
    void require_mode(DeletionMode expected) const;

    size_type capacity_;
    unsigned fingerprint_bits_;
    value_type fingerprint_mask_;
    DeletionMode deletion_mode_;
    bool use_avx2_;
    size_type size_ = 0;
    std::vector<value_type> fingerprints_;
    std::vector<size_type> reference_counts_;
    std::vector<std::uint8_t> slot_states_;
    std::unordered_set<value_type> exact_keys_;
};

}  // namespace vqf
