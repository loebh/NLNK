#pragma once

#include "vqf_common.hpp"

namespace vqf {

struct ParallelQueryOptions {
    size_type thread_count = 0;
    int numa_node = -1;
};

class ParallelQueryExecutor {
public:
    explicit ParallelQueryExecutor(ParallelQueryOptions options = {});

    template <typename Filter>
    void batch_process(const Filter& filter, const value_type* items,
                       size_type count, std::uint8_t* results) const {
        static_assert(noexcept(filter.query(value_type{})),
                      "Parallel batch queries require a noexcept query method");
        batch_process_impl(
            &filter,
            [](const void* context, value_type key) noexcept {
                return static_cast<const Filter*>(context)->query(key);
            },
            items, count, results);
    }

    size_type thread_count() const noexcept;
    int numa_node() const noexcept;

private:
    using QueryFunction = bool (*)(const void*, value_type) noexcept;

    void batch_process_impl(const void* filter, QueryFunction query,
                            const value_type* items, size_type count,
                            std::uint8_t* results) const;

    ParallelQueryOptions options_;
};

}  // namespace vqf
