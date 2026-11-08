#include "vqf/competitor_filters.hpp"

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

bool test_blocked_bloom() {
    using namespace vqf;
    BlockedBloomFilter filter(1000, 0.01);
    if (!check(filter.capacity() == 1000 && filter.memory_bytes() >= 1000,
               "Blocked Bloom capacity or memory is invalid")) {
        return false;
    }
    for (value_type key = 0; key < 900; ++key) {
        filter.insert(key);
    }
    if (!check(filter.size() == 900, "Blocked Bloom insertion count is incorrect")) {
        return false;
    }
    for (value_type key = 0; key < 900; ++key) {
        if (!check(filter.query(key), "Blocked Bloom returned a false negative")) {
            return false;
        }
    }

    size_type false_positives = 0;
    for (value_type key = 100000; key < 200000; ++key) {
        false_positives += static_cast<size_type>(filter.query(key));
    }
    if (!check(false_positives < 5000,
               "Blocked Bloom false-positive rate exceeded the broad test bound")) {
        return false;
    }

    bool rejected_rate = false;
    try {
        BlockedBloomFilter invalid(100, 1.0);
    } catch (const std::invalid_argument&) {
        rejected_rate = true;
    }
    return check(rejected_rate, "Blocked Bloom accepted an invalid false-positive rate");
}

bool test_cuckoo_filter() {
    using namespace vqf;
    CuckooFilter filter(4096);
    const size_type item_count = filter.capacity() * 95 / 100;
    std::vector<value_type> keys(item_count);

    for (size_type i = 0; i < item_count; ++i) {
        keys[i] = static_cast<value_type>(i) + 1;
        if (!check(filter.insert(keys[i]), "Cuckoo Filter failed below 95% load")) {
            return false;
        }
    }
    if (!check(filter.size() == item_count && filter.memory_bytes() > 0,
               "Cuckoo Filter size or memory accounting is invalid")) {
        return false;
    }
    for (value_type key : keys) {
        if (!check(filter.query(key), "Cuckoo Filter returned a false negative")) {
            return false;
        }
    }

    const size_type churn_count = item_count / 20;
    value_type replacement = 1000000;
    for (size_type cycle = 0; cycle < 5; ++cycle) {
        for (size_type i = 0; i < churn_count; ++i) {
            if (!check(filter.erase(keys[i]), "Cuckoo Filter failed to erase a live key")) {
                return false;
            }
            keys[i] = replacement++;
            if (!check(filter.insert(keys[i]),
                       "Cuckoo Filter failed reinsertion after erase")) {
                return false;
            }
        }
    }
    for (value_type key : keys) {
        if (!check(filter.query(key), "Cuckoo Filter lost a key after churn")) {
            return false;
        }
    }
    return check(filter.size() == item_count,
                 "Cuckoo Filter size changed after erase/reinsert churn");
}

}  // namespace

int main() {
    try {
        if (!test_blocked_bloom() || !test_cuckoo_filter()) {
            return 1;
        }
        std::cout << "[PASS] Blocked Bloom and Cuckoo Filter tests passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Unexpected exception: " << error.what() << '\n';
        return 1;
    }
}
