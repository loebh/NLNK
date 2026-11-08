#include "vqf/wide_fingerprint.hpp"

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

bool test_width(unsigned bits) {
    using namespace vqf;
    constexpr size_type capacity = 23;
    const WideKey first{1, 2, 3, 4};
    const WideKey second{5, 6, 7, 8};
    const WideKey absent{9, 10, 11, 12};

    WideFingerprintFilter fast({capacity, bits, DeletionMode::fast});
    if (!check(fast.insert(first) && fast.insert(second),
               "Wide filter failed to insert keys") ||
        !check(fast.query(first) && fast.query(second),
               "Wide filter returned a false negative") ||
        !check(fast.insert(first) && fast.size() == 3,
               "Fast mode did not count duplicate occurrences") ||
        !check(fast.erase_fast(first) && fast.query(first) && fast.size() == 2,
               "Fast erase removed a remaining duplicate") ||
        !check(fast.erase_fast(first) && fast.query(second) && fast.size() == 1,
               "Fast erase failed after the final duplicate") ||
        !check(fast.fingerprint_memory_bytes() ==
                   ((capacity * bits + 63) / 64) * sizeof(std::uint64_t) +
                       capacity * sizeof(size_type) + capacity,
               "Packed fingerprint memory size is incorrect")) {
        return false;
    }

    WideFingerprintFilter exact({capacity, bits, DeletionMode::exact});
    if (!check(exact.insert(first) && exact.insert(second) && exact.insert(first),
               "Exact mode failed to insert or deduplicate keys") ||
        !check(exact.size() == 2 && exact.exact_key_payload_bytes() == 2 * sizeof(WideKey),
               "Exact mode size or key payload accounting is incorrect") ||
        !check(!exact.erase_exact(absent) && exact.size() == 2,
               "Exact mode changed after erasing an absent key") ||
        !check(exact.erase_exact(first) && exact.query(second) && exact.size() == 1,
               "Exact erase damaged another key") ||
        !check(exact.erase_exact(second) && exact.size() == 0,
               "Exact mode failed to erase the final key")) {
        return false;
    }
    return true;
}

bool test_scan_equivalence(unsigned bits) {
    using namespace vqf;
    constexpr size_type capacity = 37;
    constexpr size_type live_count = 35;
    WideFingerprintFilter scalar(
        {capacity, bits, DeletionMode::fast, FingerprintScanMode::scalar});
    WideFingerprintFilter automatic(
        {capacity, bits, DeletionMode::fast, FingerprintScanMode::automatic});

    std::vector<WideKey> keys;
    keys.reserve(live_count);
    for (size_type i = 0; i < live_count; ++i) {
        const WideKey key{i + 1, i * 17 + 3, i * 29 + 5, i * 43 + 7};
        keys.push_back(key);
        if (!check(scalar.insert(key) && automatic.insert(key),
                   "Scalar and AVX2 scans disagreed during insert")) {
            return false;
        }
    }

    if (!check(!scalar.uses_avx2(),
               "Scalar scan mode unexpectedly enabled AVX2")) {
        return false;
    }
    for (size_type i = 0; i < keys.size(); ++i) {
        const bool scalar_hit = scalar.query(keys[i]);
        const bool automatic_hit = automatic.query(keys[i]);
        if (!check(scalar_hit == automatic_hit && automatic_hit,
                   "Scalar and AVX2 scans disagreed on a live key")) {
            std::cerr << "Fingerprint width " << bits << ", key index " << i
                      << ", scalar " << scalar_hit << ", automatic "
                      << automatic_hit << '\n';
            return false;
        }
    }

    for (size_type i = 0; i < 128; ++i) {
        const WideKey key{1000 + i, 2000 + i * 3, 3000 + i * 5, 4000 + i * 7};
        if (!check(scalar.query(key) == automatic.query(key),
                   "Scalar and AVX2 scans disagreed on an absent key")) {
            return false;
        }
    }

    for (size_type i = 0; i < 8; ++i) {
        if (!check(scalar.erase_fast(keys[i]) && automatic.erase_fast(keys[i]),
                   "Scalar and AVX2 scans disagreed during erase")) {
            return false;
        }
        const WideKey replacement{9000 + i, 10000 + i, 11000 + i, 12000 + i};
        if (!check(scalar.insert(replacement) && automatic.insert(replacement),
                   "Scalar and AVX2 scans disagreed after tombstone reuse")) {
            return false;
        }
        keys[i] = replacement;
    }

    for (const WideKey& key : keys) {
        if (!check(scalar.query(key) == automatic.query(key) &&
                       automatic.query(key),
                   "Scalar and AVX2 scans diverged after churn")) {
            return false;
        }
    }
    return true;
}

}  // namespace

int main() {
    using namespace vqf;
    try {
        for (unsigned bits : {16U, 32U, 64U, 128U}) {
            if (!test_width(bits) || !test_scan_equivalence(bits)) {
                return 1;
            }
        }

        bool rejected_width = false;
        try {
            WideFingerprintFilter invalid({10, 67, DeletionMode::fast});
        } catch (const std::invalid_argument&) {
            rejected_width = true;
        }
        if (!check(rejected_width, "Unsupported fingerprint width was accepted")) {
            return 1;
        }
        std::cout << "[PASS] Wide-key fingerprint filter tests passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Unexpected exception: " << error.what() << '\n';
        return 1;
    }
}
