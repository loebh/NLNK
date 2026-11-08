#include "vqf/competitor_filters.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

namespace vqf {
namespace {

std::uint64_t mix(std::uint64_t value) noexcept {
    value ^= value >> 30;
    value *= UINT64_C(0xbf58476d1ce4e5b9);
    value ^= value >> 27;
    value *= UINT64_C(0x94d049bb133111eb);
    value ^= value >> 31;
    return value;
}

}  // namespace

BlockedBloomFilter::BlockedBloomFilter(size_type expected_items,
                                       double target_false_positive_rate)
    : capacity_(expected_items), hash_count_(0) {
    if (expected_items == 0) {
        throw std::invalid_argument("BlockedBloomFilter expected_items must be positive");
    }
    if (!std::isfinite(target_false_positive_rate) ||
        target_false_positive_rate <= 0.0 || target_false_positive_rate >= 1.0) {
        throw std::invalid_argument(
            "BlockedBloomFilter target false-positive rate must be between 0 and 1");
    }

    const double bits_per_item =
        -std::log(target_false_positive_rate) / (std::log(2.0) * std::log(2.0));
    const double required_bits = bits_per_item * static_cast<double>(expected_items);
    const double required_blocks = std::ceil(required_bits / bits_per_block);
    if (!std::isfinite(required_blocks) ||
        required_blocks > static_cast<double>(blocks_.max_size())) {
        throw std::length_error("BlockedBloomFilter capacity is too large");
    }
    blocks_.resize(std::max<size_type>(1, static_cast<size_type>(required_blocks)));
    hash_count_ = static_cast<unsigned>(
        std::max(1.0, std::round(bits_per_item * std::log(2.0))));
}

void BlockedBloomFilter::insert(value_type key) noexcept {
    const value_type first = mix(key);
    const value_type second = mix(key ^ UINT64_C(0x9e3779b97f4a7c15)) | 1;
    const size_type block_index = static_cast<size_type>(first % blocks_.size());
    Block& block = blocks_[block_index];
    for (unsigned i = 0; i < hash_count_; ++i) {
        const size_type bit = static_cast<size_type>((first + i * second) % bits_per_block);
        block[bit / 64] |= std::uint64_t{1} << (bit % 64);
    }
    ++size_;
}

bool BlockedBloomFilter::query(value_type key) const noexcept {
    const value_type first = mix(key);
    const value_type second = mix(key ^ UINT64_C(0x9e3779b97f4a7c15)) | 1;
    const size_type block_index = static_cast<size_type>(first % blocks_.size());
    const Block& block = blocks_[block_index];
    for (unsigned i = 0; i < hash_count_; ++i) {
        const size_type bit = static_cast<size_type>((first + i * second) % bits_per_block);
        if ((block[bit / 64] & (std::uint64_t{1} << (bit % 64))) == 0) {
            return false;
        }
    }
    return true;
}

size_type BlockedBloomFilter::capacity() const noexcept {
    return capacity_;
}

size_type BlockedBloomFilter::size() const noexcept {
    return size_;
}

size_type BlockedBloomFilter::memory_bytes() const noexcept {
    return blocks_.size() * sizeof(Block);
}

unsigned BlockedBloomFilter::hash_count() const noexcept {
    return hash_count_;
}

CuckooFilter::CuckooFilter(size_type requested_slots, unsigned max_kick_attempts)
    : bucket_count_(0), max_kick_attempts_(max_kick_attempts) {
    constexpr unsigned max_supported_kicks = 500;
    if (requested_slots == 0) {
        throw std::invalid_argument("CuckooFilter requested_slots must be positive");
    }
    if (max_kick_attempts == 0 || max_kick_attempts > max_supported_kicks) {
        throw std::invalid_argument("CuckooFilter kick attempts must be between 1 and 500");
    }

    const size_type requested_buckets =
        requested_slots / bucket_size + (requested_slots % bucket_size != 0);
    bucket_count_ = 2;
    while (bucket_count_ < requested_buckets) {
        if (bucket_count_ > std::numeric_limits<size_type>::max() / 2) {
            throw std::length_error("CuckooFilter capacity is too large");
        }
        bucket_count_ *= 2;
    }
    if (bucket_count_ > buckets_.max_size()) {
        throw std::length_error("CuckooFilter capacity is too large");
    }
    buckets_.resize(bucket_count_);
}

bool CuckooFilter::insert(value_type key) {
    const std::uint16_t tag = fingerprint(key);
    const size_type first = primary_bucket(key);
    const size_type second = alternate_bucket(first, tag);
    if (bucket_insert(first, tag) || bucket_insert(second, tag)) {
        ++size_;
        return true;
    }

    using Snapshot = std::pair<size_type, Bucket>;
    std::array<Snapshot, 500> changes{};
    size_type change_count = 0;
    size_type bucket = (mix(key ^ random_state_) & 1) == 0 ? first : second;
    std::uint16_t displaced = tag;

    for (unsigned attempt = 0; attempt < max_kick_attempts_; ++attempt) {
        changes[change_count++] = {bucket, buckets_[bucket]};
        random_state_ = mix(random_state_);
        const size_type victim = static_cast<size_type>(random_state_ % bucket_size);
        std::swap(displaced, buckets_[bucket][victim]);
        bucket = alternate_bucket(bucket, displaced);
        if (bucket_insert(bucket, displaced)) {
            ++size_;
            return true;
        }
    }

    while (change_count != 0) {
        --change_count;
        buckets_[changes[change_count].first] = changes[change_count].second;
    }
    return false;
}

bool CuckooFilter::query(value_type key) const noexcept {
    const std::uint16_t tag = fingerprint(key);
    const size_type first = primary_bucket(key);
    const size_type second = alternate_bucket(first, tag);
    return bucket_contains(first, tag) || bucket_contains(second, tag);
}

bool CuckooFilter::erase(value_type key) noexcept {
    const std::uint16_t tag = fingerprint(key);
    const size_type first = primary_bucket(key);
    const size_type second = alternate_bucket(first, tag);
    if (!bucket_erase(first, tag) && !bucket_erase(second, tag)) {
        return false;
    }
    --size_;
    return true;
}

size_type CuckooFilter::capacity() const noexcept {
    return bucket_count_ * bucket_size;
}

size_type CuckooFilter::size() const noexcept {
    return size_;
}

size_type CuckooFilter::memory_bytes() const noexcept {
    return buckets_.capacity() * sizeof(Bucket);
}

std::uint16_t CuckooFilter::fingerprint(value_type key) const noexcept {
    std::uint16_t tag = static_cast<std::uint16_t>(mix(key) & 0xffff);
    return tag == 0 ? 1 : tag;
}

size_type CuckooFilter::primary_bucket(value_type key) const noexcept {
    return static_cast<size_type>(mix(key ^ UINT64_C(0xa0761d6478bd642f))) &
           (bucket_count_ - 1);
}

size_type CuckooFilter::alternate_bucket(size_type bucket,
                                         std::uint16_t tag) const noexcept {
    const size_type offset =
        (static_cast<size_type>(mix(tag ^ UINT64_C(0xe7037ed1a0b428db))) &
         (bucket_count_ - 1)) |
        size_type{1};
    return bucket ^ offset;
}

bool CuckooFilter::bucket_contains(size_type bucket,
                                   std::uint16_t tag) const noexcept {
    for (std::uint16_t entry : buckets_[bucket]) {
        if (entry == tag) {
            return true;
        }
    }
    return false;
}

bool CuckooFilter::bucket_insert(size_type bucket, std::uint16_t tag) noexcept {
    for (std::uint16_t& entry : buckets_[bucket]) {
        if (entry == 0) {
            entry = tag;
            return true;
        }
    }
    return false;
}

bool CuckooFilter::bucket_erase(size_type bucket, std::uint16_t tag) noexcept {
    for (std::uint16_t& entry : buckets_[bucket]) {
        if (entry == tag) {
            entry = 0;
            return true;
        }
    }
    return false;
}

}  // namespace vqf
