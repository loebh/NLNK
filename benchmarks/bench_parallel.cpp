#include "vqf/vqf_core.hpp"
#include "vqf/vqf_parallel.hpp"

#include <chrono>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

vqf::size_type parse_size(const char* text, const char* name, bool allow_zero) {
    try {
        const unsigned long long parsed = std::stoull(text);
        if ((!allow_zero && parsed == 0) ||
            parsed > std::numeric_limits<vqf::size_type>::max()) {
            throw std::out_of_range(name);
        }
        return static_cast<vqf::size_type>(parsed);
    } catch (const std::exception&) {
        throw std::invalid_argument(std::string("Invalid ") + name + ": " + text);
    }
}

int parse_numa_node(const char* text) {
    try {
        const long parsed = std::stol(text);
        if (parsed < -1 || parsed > std::numeric_limits<int>::max()) {
            throw std::out_of_range("NUMA node");
        }
        return static_cast<int>(parsed);
    } catch (const std::exception&) {
        throw std::invalid_argument(std::string("Invalid NUMA node: ") + text);
    }
}

template <typename Operation>
double measure_mops(vqf::size_type count, const Operation& operation) {
    const auto start = std::chrono::steady_clock::now();
    operation();
    const auto end = std::chrono::steady_clock::now();
    const std::chrono::duration<double> elapsed = end - start;
    return elapsed.count() > 0.0
               ? static_cast<double>(count) / elapsed.count() / 1e6
               : 0.0;
}

}  // namespace

int main(int argc, char** argv) {
    using namespace vqf;
    try {
        if (argc > 4) {
            throw std::invalid_argument(
                "Usage: vqf_bench_parallel [batch_count] [threads=0(auto)] [numa_node=-1]");
        }
        const size_type count =
            argc > 1 ? parse_size(argv[1], "batch count", false) : size_type{5000000};
        const size_type threads =
            argc > 2 ? parse_size(argv[2], "thread count", true) : size_type{0};
        const int node = argc > 3 ? parse_numa_node(argv[3]) : -1;

        VQFEngine engine(count);
        std::vector<value_type> items(count);
        std::vector<std::uint8_t> serial_results(count);
        std::vector<std::uint8_t> parallel_results(count);
        for (size_type i = 0; i < count; ++i) {
            items[i] = static_cast<value_type>(i) * 2 + 1;
            if ((i & 1) == 0 && !engine.insert(items[i])) {
                throw std::runtime_error("Failed to prepare the query dataset");
            }
        }

        ParallelQueryExecutor executor({threads, node});
        const double serial_mops = measure_mops(count, [&] {
            engine.batch_process(items.data(), count, serial_results.data());
        });
        const double parallel_mops = measure_mops(count, [&] {
            executor.batch_process(engine, items.data(), count, parallel_results.data());
        });
        if (serial_results != parallel_results) {
            throw std::runtime_error("Parallel results differ from serial results");
        }

        std::string worker_description;
#ifdef VQF_HAS_OPENMP
        worker_description = threads == 0 ? "OpenMP default" : std::to_string(threads);
#else
        worker_description = "serial fallback";
#endif
        std::cout << "Parallel batch-query benchmark\n"
                  << "Queries: " << count << " | workers: " << worker_description
                  << " | NUMA node: " << node << '\n'
                  << "Serial: " << serial_mops << " Mops/s\n"
                  << "Parallel: " << parallel_mops << " Mops/s\n"
                  << "Speedup: "
                  << (serial_mops > 0.0 ? parallel_mops / serial_mops : 0.0)
                  << "x\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
