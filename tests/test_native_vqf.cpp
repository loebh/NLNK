#include "vqf/vqf_native.hpp"
#include "vqf/vqf_parallel.hpp"
#include "vqf/vqf_tiered_native.hpp"

#include <iostream>
#include <stdexcept>
#include <vector>

int main() {
    try {
        if (!vqf::NativeVQF::is_supported()) {
            bool rejected_unsupported_cpu = false;
            try {
                vqf::NativeVQF unsupported(64);
            } catch (const std::runtime_error&) {
                rejected_unsupported_cpu = true;
            }
            if (!rejected_unsupported_cpu) {
                std::cerr << "NativeVQF did not reject an unsupported CPU\n";
                return 1;
            }
            std::cout << "[SKIP] Upstream VQF test requires AVX2, BMI, BMI2, and POPCNT.\n";
            return 0;
        }

        bool rejected_zero_tier_capacity = false;
        try {
            vqf::TieredNativeVQF invalid({0, 88});
        } catch (const std::invalid_argument&) {
            rejected_zero_tier_capacity = true;
        }
        bool rejected_high_tier_load = false;
        try {
            vqf::TieredNativeVQF invalid({100, 91});
        } catch (const std::invalid_argument&) {
            rejected_high_tier_load = true;
        }
        if (!rejected_zero_tier_capacity || !rejected_high_tier_load) {
            std::cerr << "Tiered VQF accepted invalid configuration\n";
            return 1;
        }

        vqf::NativeVQF filter(1024);
        const vqf::value_type first = 0x123456789abcdef0ULL;
        const vqf::value_type second = 0xfedcba9876543210ULL;

        if (filter.capacity() < 1024 || filter.size() != 0) {
            std::cerr << "VQF initialization returned invalid metadata\n";
            return 1;
        }
        if (!filter.insert(first) || !filter.insert(second) || filter.size() != 2) {
            std::cerr << "VQF failed to insert keys\n";
            return 1;
        }
        if (!filter.query(first) || !filter.query(second)) {
            std::cerr << "VQF returned a false negative\n";
            return 1;
        }
        if (!filter.erase(first) || filter.size() != 1 || filter.query(first)) {
            std::cerr << "VQF failed to remove a key\n";
            return 1;
        }
        if (!filter.query(second) || !filter.erase(second) || filter.size() != 0) {
            std::cerr << "VQF removal damaged another key or size metadata\n";
            return 1;
        }
        if (!filter.memory_bytes()) {
            std::cerr << "VQF reported zero memory usage\n";
            return 1;
        }

        vqf::NativeVQF loaded_filter(1024);
        const vqf::size_type load_count = loaded_filter.capacity() * 90 / 100;
        std::vector<vqf::value_type> keys(load_count);
        for (vqf::size_type i = 0; i < load_count; ++i) {
            keys[i] = static_cast<vqf::value_type>(i) + 1;
            if (!loaded_filter.insert(keys[i])) {
                std::cerr << "VQF failed to fill to 90% load\n";
                return 1;
            }
        }
        const vqf::size_type churn_count = loaded_filter.capacity() / 20;
        for (vqf::size_type i = 0; i < churn_count; ++i) {
            if (!loaded_filter.erase(keys[i])) {
                std::cerr << "VQF failed to erase at 90% load\n";
                return 1;
            }
            keys[i] += 100000;
            if (!loaded_filter.insert(keys[i])) {
                std::cerr << "VQF failed to reinsert at 90% load\n";
                return 1;
            }
        }
        for (vqf::value_type key : keys) {
            if (!loaded_filter.query(key)) {
                std::cerr << "VQF returned a false negative after 90% churn\n";
                return 1;
            }
        }
        if (loaded_filter.size() != load_count) {
            std::cerr << "VQF size tracking failed after 90% churn\n";
            return 1;
        }

        vqf::TieredNativeVQF tiered({1000, 88});
        const vqf::size_type tiered_base_capacity = tiered.capacity();
        const vqf::size_type tiered_item_count = tiered_base_capacity * 95 / 100;
        std::vector<vqf::value_type> tiered_keys(tiered_item_count);
        for (vqf::size_type i = 0; i < tiered_item_count; ++i) {
            tiered_keys[i] = static_cast<vqf::value_type>(i) + 1;
            if (!tiered.insert(tiered_keys[i])) {
                std::cerr << "Tiered VQF failed while expanding beyond 88% load\n";
                return 1;
            }
        }
        if (tiered.tier_count() < 2 ||
            tiered.capacity() <= tiered_base_capacity ||
            tiered.size() != tiered_item_count) {
            std::cerr << "Tiered VQF did not expand as expected\n";
            return 1;
        }
        const vqf::size_type tiered_size = tiered.size();
        if (!tiered.insert(tiered_keys.front()) || tiered.size() != tiered_size) {
            std::cerr << "Tiered VQF duplicate insertion changed its set size\n";
            return 1;
        }
        const vqf::size_type tiered_churn_count = tiered_base_capacity / 20;
        vqf::value_type next_tiered_key = 10000000;
        for (unsigned cycle = 0; cycle < 3; ++cycle) {
            for (vqf::size_type i = 0; i < tiered_churn_count; ++i) {
                if (!tiered.erase(tiered_keys[i])) {
                    std::cerr << "Tiered VQF failed to erase a tracked key\n";
                    return 1;
                }
                tiered_keys[i] = next_tiered_key++;
                if (!tiered.insert(tiered_keys[i])) {
                    std::cerr << "Tiered VQF failed during high-load reinsertion\n";
                    return 1;
                }
            }
            if (tiered.size() != tiered_item_count) {
                std::cerr << "Tiered VQF size changed during churn\n";
                return 1;
            }
        }
        for (vqf::value_type key : tiered_keys) {
            if (!tiered.query(key)) {
                std::cerr << "Tiered VQF returned a false negative after churn\n";
                return 1;
            }
        }
        if (tiered.erase(0) || tiered.key_index_payload_bytes() !=
                                  tiered.size() *
                                      (sizeof(vqf::value_type) + sizeof(vqf::size_type)) ||
            tiered.memory_bytes() < tiered.key_index_payload_bytes()) {
            std::cerr << "Tiered VQF exact key index accounting failed\n";
            return 1;
        }

        vqf::NativeVQF high_load_filter(10000);
        const vqf::size_type high_load_count = high_load_filter.capacity() * 95 / 100;
        std::vector<vqf::value_type> high_load_keys(high_load_count);
        for (vqf::size_type i = 0; i < high_load_count; ++i) {
            high_load_keys[i] = static_cast<vqf::value_type>(i) + 1;
            if (!high_load_filter.insert(high_load_keys[i])) {
                std::cerr << "VQF failed to fill to 95% load\n";
                return 1;
            }
        }
        for (vqf::value_type key : high_load_keys) {
            if (!high_load_filter.query(key)) {
                std::cerr << "VQF returned a false negative at 95% load\n";
                return 1;
            }
        }
        std::vector<std::uint8_t> parallel_results(high_load_keys.size());
#ifdef VQF_HAS_OPENMP
        vqf::ParallelQueryExecutor parallel_queries({2, -1});
#else
        vqf::ParallelQueryExecutor parallel_queries;
#endif
        parallel_queries.batch_process(high_load_filter, high_load_keys.data(),
                                       high_load_keys.size(), parallel_results.data());
        for (std::uint8_t result : parallel_results) {
            if (result == 0) {
                std::cerr << "Parallel VQF batch query returned a false negative\n";
                return 1;
            }
        }

        std::cout << "[PASS] Upstream Vector Quotient Filter API test passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Unexpected exception: " << error.what() << '\n';
        return 1;
    }
}
