#include "vqf/vqf_core.hpp"
#include "vqf/vqf_parallel.hpp"

#include <iostream>
#include <stdexcept>
#include <vector>

namespace {

bool check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        return false;
    }
    return true;
}

}  // namespace

int main() {
    using namespace vqf;
    try {
        VQFEngine engine(4096);
        std::vector<value_type> items(8192);
        std::vector<std::uint8_t> serial_results(items.size(), 0);
        std::vector<std::uint8_t> parallel_results(items.size(), 0);

        for (size_type i = 0; i < items.size(); ++i) {
            items[i] = static_cast<value_type>(i) * 17 + 3;
            if (i % 3 == 0 && !engine.insert(items[i])) {
                std::cerr << "Could not populate the baseline filter\n";
                return 1;
            }
        }
        engine.batch_process(items.data(), items.size(), serial_results.data());

#ifdef VQF_HAS_OPENMP
        constexpr size_type workers = 4;
#else
        constexpr size_type workers = 0;
#endif
        ParallelQueryExecutor executor({workers, -1});
        executor.batch_process(engine, items.data(), items.size(),
                               parallel_results.data());
        if (!check(serial_results == parallel_results,
                   "Parallel results differ from the serial batch results") ||
            !check(executor.thread_count() == workers && executor.numa_node() == -1,
                   "Parallel executor did not retain its configuration")) {
            return 1;
        }

        executor.batch_process(engine, nullptr, 0, nullptr);
        bool rejected_null_buffers = false;
        try {
            executor.batch_process(engine, nullptr, 1, parallel_results.data());
        } catch (const std::invalid_argument&) {
            rejected_null_buffers = true;
        }
        if (!check(rejected_null_buffers,
                   "Parallel batch accepted a null input buffer")) {
            return 1;
        }

        bool rejected_invalid_node = false;
        try {
            ParallelQueryExecutor invalid_node({0, -2});
        } catch (const std::invalid_argument&) {
            rejected_invalid_node = true;
        }
        if (!check(rejected_invalid_node,
                   "Parallel executor accepted an invalid NUMA node")) {
            return 1;
        }

#ifndef VQF_HAS_NUMA
        bool rejected_numa_request = false;
        try {
            ParallelQueryExecutor numa_executor({1, 0});
        } catch (const std::runtime_error&) {
            rejected_numa_request = true;
        }
        if (!check(rejected_numa_request,
                   "Build without NUMA support silently accepted NUMA pinning")) {
            return 1;
        }
#endif

#ifndef VQF_HAS_OPENMP
        bool rejected_parallel_request = false;
        try {
            ParallelQueryExecutor parallel_executor({2, -1});
        } catch (const std::runtime_error&) {
            rejected_parallel_request = true;
        }
        if (!check(rejected_parallel_request,
                   "Build without OpenMP silently accepted multiple workers")) {
            return 1;
        }
#endif

        std::cout << "[PASS] Parallel batch query tests passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Unexpected exception: " << error.what() << '\n';
        return 1;
    }
}
