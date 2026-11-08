#pragma once

#include "vqf_common.hpp"

struct vqf_filter;

namespace vqf {

class NativeVQF {
public:
    static bool is_supported() noexcept;

    explicit NativeVQF(size_type requested_slots);
    ~NativeVQF();

    NativeVQF(const NativeVQF&) = delete;
    NativeVQF& operator=(const NativeVQF&) = delete;
    NativeVQF(NativeVQF&&) = delete;
    NativeVQF& operator=(NativeVQF&&) = delete;

    bool insert(value_type key);
    bool query(value_type key) const noexcept;
    bool erase(value_type key);

    size_type capacity() const noexcept;
    size_type size() const noexcept;
    size_type memory_bytes() const noexcept;

private:
    vqf_filter* filter_;
    size_type capacity_;
    size_type size_ = 0;
};

}  // namespace vqf
