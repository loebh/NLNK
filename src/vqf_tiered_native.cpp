#include "vqf/vqf_tiered_native.hpp"

#include <limits>
#include <stdexcept>

namespace vqf {

TieredNativeVQF::TieredNativeVQF(const TieredNativeVQFConfig& config)
    : requested_tier_capacity_(config.tier_capacity),
      max_load_percent_(config.max_load_percent) {
    if (requested_tier_capacity_ == 0) {
        throw std::invalid_argument("TieredNativeVQF tier capacity must be positive");
    }
    if (max_load_percent_ < 50 || max_load_percent_ > 90) {
        throw std::invalid_argument(
            "TieredNativeVQF max load must be between 50 and 90 percent");
    }
    create_tier();
}

bool TieredNativeVQF::insert(value_type key) {
    if (key_tiers_.find(key) != key_tiers_.end()) {
        return true;
    }

    size_type tier_index = tiers_.size() - 1;
    const size_type tier_limit =
        tiers_[tier_index]->capacity() * max_load_percent_ / 100;
    bool created_tier = false;
    if (tiers_[tier_index]->size() >= tier_limit) {
        tier_index = create_tier();
        created_tier = true;
    }

    const auto index_result = key_tiers_.emplace(key, tier_index);
    if (!index_result.second) {
        return true;
    }

    try {
        if (!tiers_[tier_index]->insert(key)) {
            if (created_tier || tiers_[tier_index]->size() == 0) {
                key_tiers_.erase(index_result.first);
                if (created_tier) {
                    capacity_ -= tiers_.back()->capacity();
                    tiers_.pop_back();
                }
                return false;
            }

            tier_index = create_tier();
            created_tier = true;
            index_result.first->second = tier_index;
            if (!tiers_[tier_index]->insert(key)) {
                key_tiers_.erase(index_result.first);
                capacity_ -= tiers_.back()->capacity();
                tiers_.pop_back();
                return false;
            }
        }
    } catch (...) {
        key_tiers_.erase(index_result.first);
        if (created_tier && tiers_.back()->size() == 0) {
            capacity_ -= tiers_.back()->capacity();
            tiers_.pop_back();
        }
        throw;
    }
    ++size_;
    return true;
}

bool TieredNativeVQF::query(value_type key) const noexcept {
    for (const auto& tier : tiers_) {
        if (tier->query(key)) {
            return true;
        }
    }
    return false;
}

bool TieredNativeVQF::erase(value_type key) {
    const auto position = key_tiers_.find(key);
    if (position == key_tiers_.end()) {
        return false;
    }
    if (!tiers_[position->second]->erase(key)) {
        throw std::logic_error("TieredNativeVQF key index is inconsistent");
    }
    key_tiers_.erase(position);
    --size_;
    return true;
}

size_type TieredNativeVQF::capacity() const noexcept {
    return capacity_;
}

size_type TieredNativeVQF::size() const noexcept {
    return size_;
}

size_type TieredNativeVQF::tier_count() const noexcept {
    return tiers_.size();
}

size_type TieredNativeVQF::memory_bytes() const noexcept {
    size_type bytes = key_index_payload_bytes();
    for (const auto& tier : tiers_) {
        bytes += tier->memory_bytes();
    }
    return bytes;
}

size_type TieredNativeVQF::key_index_payload_bytes() const noexcept {
    return key_tiers_.size() * (sizeof(value_type) + sizeof(size_type));
}

unsigned TieredNativeVQF::max_load_percent() const noexcept {
    return max_load_percent_;
}

size_type TieredNativeVQF::create_tier() {
    std::unique_ptr<NativeVQF> tier =
        std::make_unique<NativeVQF>(requested_tier_capacity_);
    if (tier->capacity() > std::numeric_limits<size_type>::max() - capacity_) {
        throw std::length_error("TieredNativeVQF total capacity overflows size_type");
    }
    const size_type actual_capacity = tier->capacity();
    tiers_.push_back(std::move(tier));
    capacity_ += actual_capacity;
    return tiers_.size() - 1;
}

}  // namespace vqf
