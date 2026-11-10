#include "vqf/vqf_filter.hpp"

#include "fingerprint_scan.hpp"

#include <limits>
#include <stdexcept>

namespace vqf {
using namespace std;

constexpr std::uint8_t FingerprintFilter::empty_slot;
constexpr std::uint8_t FingerprintFilter::occupied_slot;
constexpr std::uint8_t FingerprintFilter::deleted_slot;

namespace {

value_type mix(value_type value) noexcept {
    value ^= value >> 30;
    value *= UINT64_C(0xbf58476d1ce4e5b9);
    value ^= value >> 27;
    value *= UINT64_C(0x94d049bb133111eb);
    value ^= value >> 31;
    return value;
}

value_type make_fingerprint_mask(unsigned bits) {
    if (bits == 0 || bits > 64) {
        throw invalid_argument("fingerprint_bits must be between 1 and 64");
    }
    return bits == 64 ? numeric_limits<value_type>::max()
                      : (value_type{1} << bits) - 1;
}

}  // namespace

FingerprintFilter::FingerprintFilter(const FingerprintFilterConfig& config)
    : capacity_(config.capacity),
      fingerprint_bits_(config.fingerprint_bits),
      fingerprint_mask_(make_fingerprint_mask(config.fingerprint_bits)),
      deletion_mode_(config.deletion_mode),
      use_avx2_(config.scan_mode == FingerprintScanMode::automatic &&
                detail::avx2_scan_available()),
      fingerprints_(config.capacity, 0),
      reference_counts_(config.capacity, 0),
      slot_states_(config.capacity, empty_slot) {
    if (capacity_ == 0) {
        throw invalid_argument("FingerprintFilter capacity must be greater than zero");
    }
    if (deletion_mode_ == DeletionMode::exact) {
        exact_keys_.reserve(capacity_);
    }
}

bool FingerprintFilter::insert(value_type key) {
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

    if (size_ == capacity_) {
        return false;
    }
    if (!add_fingerprint(fingerprint(key))) {
        return false;
    }
    ++size_;
    return true;
}

bool FingerprintFilter::query(value_type key) const noexcept {
    return find_fingerprint(fingerprint(key)) != capacity_;
}

bool FingerprintFilter::erase_fast(value_type key) {
    require_mode(DeletionMode::fast);
    const size_type slot = find_fingerprint(fingerprint(key));
    if (slot == capacity_) {
        return false;
    }
    remove_fingerprint(slot);
    --size_;
    return true;
}

bool FingerprintFilter::erase_exact(value_type key) {
    require_mode(DeletionMode::exact);
    const auto key_position = exact_keys_.find(key);
    if (key_position == exact_keys_.end()) {
        return false;
    }

    const size_type slot = find_fingerprint(fingerprint(key));
    if (slot == capacity_) {
        throw logic_error("FingerprintFilter exact-key index is inconsistent");
    }

    remove_fingerprint(slot);
    exact_keys_.erase(key_position);
    --size_;
    return true;
}

size_type FingerprintFilter::capacity() const noexcept {
    return capacity_;
}

size_type FingerprintFilter::size() const noexcept {
    return size_;
}

DeletionMode FingerprintFilter::deletion_mode() const noexcept {
    return deletion_mode_;
}

bool FingerprintFilter::uses_avx2() const noexcept {
    return use_avx2_;
}

value_type FingerprintFilter::fingerprint(value_type key) const noexcept {
    return mix(key) & fingerprint_mask_;
}

size_type FingerprintFilter::find_fingerprint(value_type value) const noexcept {
    const size_type start = static_cast<size_type>(mix(value) % capacity_);
    if (use_avx2_) {
        const detail::FingerprintProbeResult result =
            detail::probe_fingerprint_avx2(
                reinterpret_cast<const uint8_t*>(fingerprints_.data()),
                slot_states_.data(), capacity_, start, value, 0, 64);
        return result.found_slot;
    }

    size_type slot = start;
    for (size_type probes = 0; probes < capacity_; ++probes) {
        if (slot_states_[slot] == empty_slot) {
            return capacity_;
        }
        if (slot_states_[slot] == occupied_slot && fingerprints_[slot] == value) {
            return slot;
        }
        slot = (slot + 1 == capacity_) ? 0 : slot + 1;
    }
    return capacity_;
}

bool FingerprintFilter::add_fingerprint(value_type value) noexcept {
    if (use_avx2_) {
        const size_type start = static_cast<size_type>(mix(value) % capacity_);
        const detail::FingerprintProbeResult result =
            detail::probe_fingerprint_avx2(
                reinterpret_cast<const uint8_t*>(fingerprints_.data()),
                slot_states_.data(), capacity_, start, value, 0, 64);
        if (result.found_slot != capacity_) {
            ++reference_counts_[result.found_slot];
            return true;
        }
        if (result.insertion_slot == capacity_) {
            return false;
        }
        fingerprints_[result.insertion_slot] = value;
        reference_counts_[result.insertion_slot] = 1;
        slot_states_[result.insertion_slot] = occupied_slot;
        return true;
    }

    size_type slot = static_cast<size_type>(mix(value) % capacity_);
    size_type first_deleted = capacity_;

    for (size_type probes = 0; probes < capacity_; ++probes) {
        if (slot_states_[slot] == occupied_slot) {
            if (fingerprints_[slot] == value) {
                ++reference_counts_[slot];
                return true;
            }
        } else if (slot_states_[slot] == deleted_slot) {
            if (first_deleted == capacity_) {
                first_deleted = slot;
            }
        } else {
            const size_type target = first_deleted == capacity_ ? slot : first_deleted;
            fingerprints_[target] = value;
            reference_counts_[target] = 1;
            slot_states_[target] = occupied_slot;
            return true;
        }
        slot = (slot + 1 == capacity_) ? 0 : slot + 1;
    }

    if (first_deleted != capacity_) {
        fingerprints_[first_deleted] = value;
        reference_counts_[first_deleted] = 1;
        slot_states_[first_deleted] = occupied_slot;
        return true;
    }
    return false;
}

void FingerprintFilter::remove_fingerprint(size_type slot) noexcept {
    if (--reference_counts_[slot] == 0) {
        slot_states_[slot] = deleted_slot;
    }
}

void FingerprintFilter::require_mode(DeletionMode expected) const {
    if (deletion_mode_ != expected) {
        throw logic_error("Deletion operation does not match the filter's configured mode");
    }
}

}  // namespace vqf
