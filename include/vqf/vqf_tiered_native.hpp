#pragma once

#include "vqf_native.hpp"

#include <memory>
#include <unordered_map>
#include <vector>

namespace vqf {

struct TieredNativeVQFConfig {
    size_type tier_capacity;
    unsigned max_load_percent = 88;
};

class TieredNativeVQF {
public:
    explicit TieredNativeVQF(const TieredNativeVQFConfig& config);

    bool insert(value_type key);
    bool query(value_type key) const noexcept;
    bool erase(value_type key);

    size_type capacity() const noexcept;
    size_type size() const noexcept;
    size_type tier_count() const noexcept;
    size_type memory_bytes() const noexcept;
    size_type key_index_payload_bytes() const noexcept;
    unsigned max_load_percent() const noexcept;

private:
    size_type create_tier();

    size_type requested_tier_capacity_;
    unsigned max_load_percent_;
    size_type size_ = 0;
    size_type capacity_ = 0;
    std::vector<std::unique_ptr<NativeVQF>> tiers_;
    std::unordered_map<value_type, size_type> key_tiers_;
};

}  // namespace vqf
