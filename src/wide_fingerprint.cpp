#include "vqf/wide_fingerprint.hpp"

#include "fingerprint_scan.hpp"

#include <limits>
#include <stdexcept>

namespace vqf {
using namespace std;
namespace {

uint64_t mix(uint64_t value) noexcept {
    value ^= value >> 30;
    value *= UINT64_C(0xbf58476d1ce4e5b9);
    value ^= value >> 27;
    value *= UINT64_C(0x94d049bb133111eb);
    value ^= value >> 31;
    return value;
}

array<uint64_t, 2> digest(const WideKey& key) noexcept {
    const uint64_t combined =
        mix(key[0] ^ mix(key[1] + UINT64_C(0x9e3779b97f4a7c15)) ^
            mix(key[2] + UINT64_C(0xd1b54a32d192ed03)) ^
            mix(key[3] + UINT64_C(0x94d049bb133111eb)));
    return {mix(combined ^ UINT64_C(0xa0761d6478bd642f)),
            mix(combined ^ UINT64_C(0xe7037ed1a0b428db))};
}

uint64_t low_mask(unsigned bits) noexcept {
    return bits == 64 ? numeric_limits<uint64_t>::max()
                      : (uint64_t{1} << bits) - 1;
}

}  // namespace

size_type WideKeyHash::operator()(const WideKey& key) const noexcept {
    const auto value = digest(key);
    return static_cast<size_type>(value[0] ^ value[1]);
}

WideFingerprintFilter::WideFingerprintFilter(const WideFingerprintFilterConfig& config)
    : capacity_(config.capacity),
      fingerprint_bits_(config.fingerprint_bits),
      deletion_mode_(config.deletion_mode),
      use_avx2_(config.scan_mode == FingerprintScanMode::automatic &&
                detail::avx2_scan_available()) {
    if (fingerprint_bits_ != 16 && fingerprint_bits_ != 32 &&
        fingerprint_bits_ != 64 && fingerprint_bits_ != 128) {
        throw invalid_argument("fingerprint_bits must be 16, 32, 64, or 128");
    }
    if (capacity_ == 0) {
        throw invalid_argument("WideFingerprintFilter capacity must be greater than zero");
    }
    if (capacity_ > (numeric_limits<size_type>::max() - 63) / fingerprint_bits_) {
        throw length_error("WideFingerprintFilter capacity is too large");
    }

    const size_type total_bits = capacity_ * fingerprint_bits_;
    const size_type word_count = total_bits / 64 + (total_bits % 64 != 0);
    fingerprints_.assign(word_count, 0);
    reference_counts_.assign(capacity_, 0);
    slot_states_.assign(capacity_, empty_slot);
    if (deletion_mode_ == DeletionMode::exact) {
        exact_keys_.reserve(capacity_);
    }
}

bool WideFingerprintFilter::insert(const WideKey& key) {
    if (deletion_mode_ == DeletionMode::exact) {
        const auto result = exact_keys_.insert(key);
        if (!result.second) {
            return true;
        }
        if (size_ == capacity_) {
            exact_keys_.erase(result.first);
            return false;
        }
        if (!add_fingerprint(fingerprint(key))) {
            exact_keys_.erase(result.first);
            return false;
        }
        ++size_;
        return true;
    }

    if (size_ == capacity_ || !add_fingerprint(fingerprint(key))) {
        return false;
    }
    ++size_;
    return true;
}

bool WideFingerprintFilter::query(const WideKey& key) const noexcept {
    return find_fingerprint(fingerprint(key)) != capacity_;
}

bool WideFingerprintFilter::erase_fast(const WideKey& key) {
    require_mode(DeletionMode::fast);
    const size_type slot = find_fingerprint(fingerprint(key));
    if (slot == capacity_) {
        return false;
    }
    remove_fingerprint(slot);
    --size_;
    return true;
}

bool WideFingerprintFilter::erase_exact(const WideKey& key) {
    require_mode(DeletionMode::exact);
    const auto key_position = exact_keys_.find(key);
    if (key_position == exact_keys_.end()) {
        return false;
    }

    const size_type slot = find_fingerprint(fingerprint(key));
    if (slot == capacity_) {
        throw logic_error("WideFingerprintFilter exact-key index is inconsistent");
    }

    remove_fingerprint(slot);
    exact_keys_.erase(key_position);
    --size_;
    return true;
}

size_type WideFingerprintFilter::capacity() const noexcept {
    return capacity_;
}

size_type WideFingerprintFilter::size() const noexcept {
    return size_;
}

size_type WideFingerprintFilter::fingerprint_memory_bytes() const noexcept {
    return fingerprints_.capacity() * sizeof(uint64_t) +
           reference_counts_.capacity() * sizeof(size_type) +
           slot_states_.capacity() * sizeof(uint8_t);
}

size_type WideFingerprintFilter::exact_key_payload_bytes() const noexcept {
    return exact_keys_.size() * sizeof(WideKey);
}

bool WideFingerprintFilter::uses_avx2() const noexcept {
    return use_avx2_;
}

WideFingerprintFilter::Fingerprint
WideFingerprintFilter::fingerprint(const WideKey& key) const noexcept {
    const auto value = digest(key);
    Fingerprint result{value[0], value[1]};

    if (fingerprint_bits_ < 64) {
        result.low &= low_mask(fingerprint_bits_);
        result.high = 0;
    } else if (fingerprint_bits_ == 64) {
        result.high = 0;
    }
    return result;
}

WideFingerprintFilter::Fingerprint
WideFingerprintFilter::read_fingerprint(size_type slot) const noexcept {
    const size_type bit_position = slot * fingerprint_bits_;
    const size_type word = bit_position / 64;
    const unsigned offset = static_cast<unsigned>(bit_position % 64);
    Fingerprint result;

    if (fingerprint_bits_ == 128) {
        result.low = fingerprints_[word];
        result.high = fingerprints_[word + 1];
    } else if (fingerprint_bits_ <= 64) {
        const uint64_t mask = low_mask(fingerprint_bits_);
        if (offset + fingerprint_bits_ <= 64) {
            result.low = (fingerprints_[word] >> offset) & mask;
        } else {
            result.low = (fingerprints_[word] >> offset) |
                         (fingerprints_[word + 1] << (64 - offset));
            result.low &= mask;
        }
    }
    return result;
}

void WideFingerprintFilter::write_fingerprint(size_type slot,
                                              Fingerprint value) noexcept {
    const size_type bit_position = slot * fingerprint_bits_;
    const size_type word = bit_position / 64;
    const unsigned offset = static_cast<unsigned>(bit_position % 64);

    if (fingerprint_bits_ == 128) {
        fingerprints_[word] = value.low;
        fingerprints_[word + 1] = value.high;
    } else if (fingerprint_bits_ <= 64) {
        const uint64_t mask = low_mask(fingerprint_bits_);
        if (offset + fingerprint_bits_ <= 64) {
            const uint64_t shifted_mask = mask << offset;
            fingerprints_[word] =
                (fingerprints_[word] & ~shifted_mask) | ((value.low & mask) << offset);
        } else {
            const unsigned first_bits = 64 - offset;
            const uint64_t first_mask = low_mask(first_bits);
            const uint64_t second_mask = low_mask(fingerprint_bits_ - first_bits);
            fingerprints_[word] =
                (fingerprints_[word] & ~(first_mask << offset)) |
                ((value.low & first_mask) << offset);
            fingerprints_[word + 1] =
                (fingerprints_[word + 1] & ~second_mask) |
                ((value.low >> first_bits) & second_mask);
        }
    }
}

size_type WideFingerprintFilter::find_fingerprint(Fingerprint value) const noexcept {
    const size_type start = static_cast<size_type>(WideKeyHash{}(
                                WideKey{value.low, value.high, value.low, value.high})) %
                            capacity_;
    if (use_avx2_) {
        const detail::FingerprintProbeResult result =
            detail::probe_fingerprint_avx2(
                reinterpret_cast<const uint8_t*>(fingerprints_.data()),
                slot_states_.data(), capacity_, start, value.low, value.high,
                fingerprint_bits_);
        return result.found_slot;
    }

    size_type slot = start;
    for (size_type probes = 0; probes < capacity_; ++probes) {
        if (slot_states_[slot] == empty_slot) {
            return capacity_;
        }
        if (slot_states_[slot] == occupied_slot && read_fingerprint(slot) == value) {
            return slot;
        }
        slot = (slot + 1 == capacity_) ? 0 : slot + 1;
    }
    return capacity_;
}

bool WideFingerprintFilter::add_fingerprint(Fingerprint value) noexcept {
    const size_type start = static_cast<size_type>(WideKeyHash{}(
                                WideKey{value.low, value.high, value.low, value.high})) %
                            capacity_;
    if (use_avx2_) {
        const detail::FingerprintProbeResult result =
            detail::probe_fingerprint_avx2(
                reinterpret_cast<const uint8_t*>(fingerprints_.data()),
                slot_states_.data(), capacity_, start, value.low, value.high,
                fingerprint_bits_);
        if (result.found_slot != capacity_) {
            ++reference_counts_[result.found_slot];
            return true;
        }
        if (result.insertion_slot == capacity_) {
            return false;
        }
        write_fingerprint(result.insertion_slot, value);
        reference_counts_[result.insertion_slot] = 1;
        slot_states_[result.insertion_slot] = occupied_slot;
        return true;
    }

    size_type slot = start;
    size_type first_deleted = capacity_;

    for (size_type probes = 0; probes < capacity_; ++probes) {
        if (slot_states_[slot] == occupied_slot) {
            if (read_fingerprint(slot) == value) {
                ++reference_counts_[slot];
                return true;
            }
        } else if (slot_states_[slot] == deleted_slot) {
            if (first_deleted == capacity_) {
                first_deleted = slot;
            }
        } else {
            const size_type target = first_deleted == capacity_ ? slot : first_deleted;
            write_fingerprint(target, value);
            reference_counts_[target] = 1;
            slot_states_[target] = occupied_slot;
            return true;
        }
        slot = (slot + 1 == capacity_) ? 0 : slot + 1;
    }

    if (first_deleted != capacity_) {
        write_fingerprint(first_deleted, value);
        reference_counts_[first_deleted] = 1;
        slot_states_[first_deleted] = occupied_slot;
        return true;
    }
    return false;
}

void WideFingerprintFilter::remove_fingerprint(size_type slot) noexcept {
    if (--reference_counts_[slot] == 0) {
        slot_states_[slot] = deleted_slot;
    }
}

void WideFingerprintFilter::require_mode(DeletionMode expected) const {
    if (deletion_mode_ != expected) {
        throw logic_error("Deletion operation does not match the filter's configured mode");
    }
}

}  // namespace vqf
