#pragma once

#include "vqf_common.hpp"

#include <array>
#include <cstdint>
#include <vector>

namespace vqf {

class BlockedBloomFilter {
public:
    BlockedBloomFilter(size_type expected_items, double target_false_positive_rate);

    void insert(value_type key) noexcept;
    bool query(value_type key) const noexcept;

    size_type capacity() const noexcept;
    size_type size() const noexcept;
    size_type memory_bytes() const noexcept;
    unsigned hash_count() const noexcept;

private:
    static constexpr size_type bits_per_block = 512;
    using Block = std::array<std::uint64_t, bits_per_block / 64>;

    size_type capacity_;
    size_type size_ = 0;
    unsigned hash_count_;
    std::vector<Block> blocks_;
};

class CuckooFilter {
public:
    explicit CuckooFilter(size_type requested_slots,
                          unsigned max_kick_attempts = 500);

    bool insert(value_type key);
    bool query(value_type key) const noexcept;
    // Like other fingerprint filters, erase only keys known to be live.
    bool erase(value_type key) noexcept;

    size_type capacity() const noexcept;
    size_type size() const noexcept;
    size_type memory_bytes() const noexcept;

private:
    static constexpr size_type bucket_size = 4;
    using Bucket = std::array<std::uint16_t, bucket_size>;

    std::uint16_t fingerprint(value_type key) const noexcept;
    size_type primary_bucket(value_type key) const noexcept;
    size_type alternate_bucket(size_type bucket, std::uint16_t fingerprint) const noexcept;
    bool bucket_contains(size_type bucket, std::uint16_t fingerprint) const noexcept;
    bool bucket_insert(size_type bucket, std::uint16_t fingerprint) noexcept;
    bool bucket_erase(size_type bucket, std::uint16_t fingerprint) noexcept;

    size_type bucket_count_;
    size_type size_ = 0;
    unsigned max_kick_attempts_;
    std::vector<Bucket> buckets_;
    std::uint64_t random_state_ = UINT64_C(0x9e3779b97f4a7c15);
};

}  // namespace vqf
