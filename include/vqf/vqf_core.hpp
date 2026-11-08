#pragma once

#include "vqf_common.hpp"

#include <cstdint>
#include <vector>

namespace vqf {

class VQFEngine {
public:
    explicit VQFEngine(size_type capacity);

    bool insert(value_type item);
    bool query(value_type item) const noexcept;
    void batch_process(const value_type* items, size_type count,
                       std::uint8_t* results) const;

    size_type capacity() const noexcept;
    size_type size() const noexcept;

private:
    size_type find_slot(value_type item) const noexcept;

    size_type capacity_;
    size_type size_ = 0;
    std::vector<value_type> storage_;
    std::vector<std::uint8_t> occupied_;
};

}  // namespace vqf
