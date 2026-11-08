#pragma once

#include "vqf_filter.hpp"

#include <array>
#include <cstdint>
#include <unordered_set>
#include <vector>

namespace vqf {

using WideKey = std::array<std::uint64_t, 4>;

struct WideKeyHash {
    size_type operator()(const WideKey& key) const noexcept;
};

struct WideFingerprintFilterConfig {
    size_type capacity;
    unsigned fingerprint_bits = 64;
    DeletionMode deletion_mode = DeletionMode::fast;
    FingerprintScanMode scan_mode = FingerprintScanMode::scalar;
};

class WideFingerprintFilter {
public:
    explicit WideFingerprintFilter(const WideFingerprintFilterConfig& config);

    bool insert(const WideKey& key);
    bool query(const WideKey& key) const noexcept;
    bool erase_fast(const WideKey& key);
    bool erase_exact(const WideKey& key);

    size_type capacity() const noexcept;
    size_type size() const noexcept;
    size_type fingerprint_memory_bytes() const noexcept;
    size_type exact_key_payload_bytes() const noexcept;
    bool uses_avx2() const noexcept;

private:
    struct Fingerprint {
        std::uint64_t low = 0;
        std::uint64_t high = 0;

        bool operator==(const Fingerprint& other) const noexcept {
            return low == other.low && high == other.high;
        }
    };

    static constexpr std::uint8_t empty_slot = 0;
    static constexpr std::uint8_t occupied_slot = 1;
    static constexpr std::uint8_t deleted_slot = 2;

    Fingerprint fingerprint(const WideKey& key) const noexcept;
    Fingerprint read_fingerprint(size_type slot) const noexcept;
    void write_fingerprint(size_type slot, Fingerprint value) noexcept;
    size_type find_fingerprint(Fingerprint value) const noexcept;
    bool add_fingerprint(Fingerprint value) noexcept;
    void remove_fingerprint(size_type slot) noexcept;
    void require_mode(DeletionMode expected) const;

    size_type capacity_;
    unsigned fingerprint_bits_;
    DeletionMode deletion_mode_;
    bool use_avx2_;
    size_type size_ = 0;
    std::vector<std::uint64_t> fingerprints_;
    std::vector<size_type> reference_counts_;
    std::vector<std::uint8_t> slot_states_;
    std::unordered_set<WideKey, WideKeyHash> exact_keys_;
};

}  // namespace vqf
