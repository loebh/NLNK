#include "vqf/vqf_native.hpp"

#include "vqf_filter.h"

#include <cstdlib>
#include <stdexcept>

namespace vqf {
namespace {

value_type hash_key(value_type value) noexcept {
    value ^= value >> 30;
    value *= UINT64_C(0xbf58476d1ce4e5b9);
    value ^= value >> 27;
    value *= UINT64_C(0x94d049bb133111eb);
    value ^= value >> 31;
    return value;
}

}  // namespace

bool NativeVQF::is_supported() noexcept {
#if (defined(__GNUC__) || defined(__clang__)) && \
    (defined(__x86_64__) || defined(__i386__))
    __builtin_cpu_init();
    return __builtin_cpu_supports("avx2") &&
           __builtin_cpu_supports("bmi") &&
           __builtin_cpu_supports("bmi2") &&
           __builtin_cpu_supports("popcnt");
#else
    return false;
#endif
}

NativeVQF::NativeVQF(size_type requested_slots)
    : filter_(nullptr), capacity_(0) {
    if (requested_slots == 0) {
        throw std::invalid_argument("NativeVQF slot count must be greater than zero");
    }
    if (!is_supported()) {
        throw std::runtime_error(
            "NativeVQF requires x86 AVX2, BMI, BMI2, and POPCNT; "
            "this upstream build has no scalar fallback");
    }

    filter_ = vqf_init(static_cast<std::uint64_t>(requested_slots));
    if (filter_ == nullptr) {
        throw std::bad_alloc();
    }
    capacity_ = static_cast<size_type>(filter_->metadata.nslots);
}

NativeVQF::~NativeVQF() {
    std::free(filter_);
}

bool NativeVQF::insert(value_type key) {
    if (!vqf_insert(filter_, hash_key(key))) {
        return false;
    }
    ++size_;
    return true;
}

bool NativeVQF::query(value_type key) const noexcept {
    return vqf_is_present(filter_, hash_key(key));
}

bool NativeVQF::erase(value_type key) {
    if (!vqf_remove(filter_, hash_key(key))) {
        return false;
    }
    --size_;
    return true;
}

size_type NativeVQF::capacity() const noexcept {
    return capacity_;
}

size_type NativeVQF::size() const noexcept {
    return size_;
}

size_type NativeVQF::memory_bytes() const noexcept {
    return static_cast<size_type>(filter_->metadata.total_size_in_bytes) +
           sizeof(vqf_metadata);
}

}  // namespace vqf
