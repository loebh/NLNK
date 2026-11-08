#include "vqf/vqf_core.hpp"

#include <chrono>
#include <cstdint>
#include <iostream>
#include <vector>

int main() {
    constexpr vqf::size_type item_count = 1'000'000;
    vqf::VQFEngine engine(item_count);
    std::vector<vqf::value_type> items(item_count);

    for (vqf::size_type i = 0; i < item_count; ++i) {
        items[i] = static_cast<vqf::value_type>(i) * 2 + 1;
    }

    const auto insert_start = std::chrono::steady_clock::now();
    for (vqf::size_type i = 0; i < item_count; ++i) {
        if (!engine.insert(items[i])) {
            std::cerr << "Insert failed at item " << i << '\n';
            return 1;
        }
    }
    const auto insert_end = std::chrono::steady_clock::now();

    std::vector<std::uint8_t> results(item_count);
    const auto query_start = std::chrono::steady_clock::now();
    engine.batch_process(items.data(), item_count, results.data());
    const auto query_end = std::chrono::steady_clock::now();

    for (std::uint8_t result : results) {
        if (result == 0) {
            std::cerr << "Batch query missed an inserted item\n";
            return 1;
        }
    }

    const std::chrono::duration<double> insert_time = insert_end - insert_start;
    const std::chrono::duration<double> query_time = query_end - query_start;
    const double insert_mops = item_count / insert_time.count() / 1e6;
    const double query_mops = item_count / query_time.count() / 1e6;

    std::cout << "Baseline insert: " << insert_mops << " Mops/sec\n"
              << "Baseline batch query: " << query_mops << " Mops/sec\n";
    return 0;
}
