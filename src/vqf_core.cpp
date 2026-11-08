#include "vqf/vqf_core.hpp"

#include <stdexcept>

namespace vqf {
namespace {

value_type hash_value(value_type value) noexcept {
    value ^= value >> 30;
    value *= UINT64_C(0xbf58476d1ce4e5b9);
    value ^= value >> 27;
    value *= UINT64_C(0x94d049bb133111eb);
    value ^= value >> 31;
    return value;
}

}  // namespace

VQFEngine::VQFEngine(size_type capacity)
    : capacity_(capacity), storage_(capacity), occupied_(capacity, 0) {
    if (capacity == 0) {
        throw std::invalid_argument("VQFEngine capacity must be greater than zero");
    }
}

bool VQFEngine::insert(value_type item) {
    const size_type slot = find_slot(item);
    if (slot == capacity_) {
        return false;
    }
    if (occupied_[slot] != 0) {
        return true;
    }

    storage_[slot] = item;
    occupied_[slot] = 1;
    ++size_;
    return true;
}

bool VQFEngine::query(value_type item) const noexcept {
    const size_type slot = find_slot(item);
    return slot != capacity_ && occupied_[slot] != 0;
}

void VQFEngine::batch_process(const value_type* items, size_type count,
                              std::uint8_t* results) const {
    if (count != 0 && (items == nullptr || results == nullptr)) {
        throw std::invalid_argument("batch_process requires non-null buffers");
    }
    for (size_type i = 0; i < count; ++i) {
        results[i] = static_cast<std::uint8_t>(query(items[i]));
    }
}

size_type VQFEngine::capacity() const noexcept {
    return capacity_;
}

size_type VQFEngine::size() const noexcept {
    return size_;
}

size_type VQFEngine::find_slot(value_type item) const noexcept {
    size_type slot = static_cast<size_type>(hash_value(item) % capacity_);
    for (size_type probes = 0; probes < capacity_; ++probes) {
        if (occupied_[slot] == 0 || storage_[slot] == item) {
            return slot;
        }
        slot = (slot + 1 == capacity_) ? 0 : slot + 1;
    }
    return capacity_;
}

}  // namespace vqf
