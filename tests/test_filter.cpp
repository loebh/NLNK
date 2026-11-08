#include "vqf/vqf_filter.hpp"

#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
using namespace std;

vqf::value_type mix(vqf::value_type value) {
    value ^= value >> 30;
    value *= UINT64_C(0xbf58476d1ce4e5b9);
    value ^= value >> 27;
    value *= UINT64_C(0x94d049bb133111eb);
    value ^= value >> 31;
    return value;
}

bool check(bool condition, const char* message) {
    if (!condition) {
        cerr << message << '\n';
        return false;
    }
    return true;
}

vector<vqf::value_type> find_probe_chain(vqf::size_type capacity,
                                        unsigned fingerprint_bits,
                                        vqf::size_type count) {
    using namespace vqf;
    const value_type mask = (value_type{1} << fingerprint_bits) - 1;
    const size_type target_bucket = 0;
    vector<value_type> fingerprints;
    for (value_type key = 0; fingerprints.size() < count; ++key) {
        const value_type fp = mix(key) & mask;
        if (mix(fp) % capacity == target_bucket &&
            find(fingerprints.begin(), fingerprints.end(), fp) == fingerprints.end()) {
            fingerprints.push_back(fp);
        }
    }

    vector<value_type> keys;
    for (value_type fp : fingerprints) {
        for (value_type key = 0;; ++key) {
            if ((mix(key) & mask) == fp) {
                keys.push_back(key);
                break;
            }
        }
    }
    return keys;
}

bool test_collision_deletion(vqf::DeletionMode mode) {
    using namespace vqf;
    const bool exact = mode == DeletionMode::exact;
    FingerprintFilter filter({16, 8, mode});
    const vector<value_type> keys = find_probe_chain(16, 8, 3);

    for (value_type key : keys) {
        if (!check(filter.insert(key), "Could not insert colliding probe-chain key")) {
            return false;
        }
    }
    const bool erased = exact ? filter.erase_exact(keys[0]) : filter.erase_fast(keys[0]);
    if (!check(erased, "Could not erase first key in probe chain")) {
        return false;
    }
    for (size_type i = 1; i < keys.size(); ++i) {
        if (!check(filter.query(keys[i]),
                   "Erasing a slot broke lookup beyond a tombstone")) {
            return false;
        }
    }
    return true;
}

bool test_high_load_churn(vqf::DeletionMode mode) {
    using namespace vqf;
    constexpr size_type capacity = 400;
    constexpr size_type initial_items = capacity * 95 / 100;
    constexpr size_type churn_items = capacity / 20;
    constexpr size_type cycles = 20;
    const bool exact = mode == DeletionMode::exact;
    FingerprintFilter filter({capacity, 24, mode});
    vector<value_type> live_keys(initial_items);

    for (size_type i = 0; i < initial_items; ++i) {
        live_keys[i] = static_cast<value_type>(i) + 1;
        if (!check(filter.insert(live_keys[i]), "Insert failed while filling to 95%")) {
            return false;
        }
    }

    value_type next_key = 100000;
    for (size_type cycle = 0; cycle < cycles; ++cycle) {
        for (size_type i = 0; i < churn_items; ++i) {
            const bool erased = exact ? filter.erase_exact(live_keys[i])
                                      : filter.erase_fast(live_keys[i]);
            if (!check(erased, "Erase failed during 95% load churn")) {
                return false;
            }
            live_keys[i] = next_key++;
            if (!check(filter.insert(live_keys[i]),
                       "Insert failed after erase at 95% load")) {
                return false;
            }
        }

        if (!check(filter.size() == initial_items,
                   "Logical size changed during 95% load churn")) {
            return false;
        }
        for (value_type key : live_keys) {
            if (!check(filter.query(key),
                       "Live key became absent during 95% load churn")) {
                return false;
            }
        }
    }
    return true;
}

}  // namespace

int main() {
    using namespace std;
    using namespace vqf;
    try {
        value_type first = 0;
        value_type second = 0;
        value_type absent = 0;
        bool found_pair = false;
        bool found_absent = false;

        for (value_type candidate = 1; candidate < 100; ++candidate) {
            if ((mix(candidate) & 1) != (mix(first) & 1)) {
                if (!found_pair) {
                    second = candidate;
                    found_pair = true;
                }
            } else if (candidate != first && !found_absent) {
                absent = candidate;
                found_absent = true;
            }
        }
        if (!check(found_pair && found_absent, "Could not generate fingerprint collision keys")) {
            return 1;
        }

        FingerprintFilter fast({16, 1, DeletionMode::fast});
        if (!check(fast.insert(first) && fast.insert(second),
                   "Fast mode failed to insert colliding keys") ||
            !check(fast.erase_fast(first), "Fast mode failed to erase inserted key") ||
            !check(fast.query(second) && fast.size() == 1,
                   "Fast erase removed a colliding live key") ||
            !check(fast.erase_fast(second) && !fast.query(first) && fast.size() == 0,
                   "Fast mode failed after the final colliding key was erased")) {
            return 1;
        }

        FingerprintFilter scalar_scan(
            {64, 32, DeletionMode::fast, FingerprintScanMode::scalar});
        FingerprintFilter automatic_scan(
            {64, 32, DeletionMode::fast, FingerprintScanMode::automatic});
        if (!check(!scalar_scan.uses_avx2(),
                   "Scalar scan mode unexpectedly enabled AVX2")) {
            return 1;
        }
        for (value_type key = 1; key <= 60; ++key) {
            if (!check(scalar_scan.insert(key) && automatic_scan.insert(key),
                       "Fingerprint scan backends disagreed during insert")) {
                return 1;
            }
        }
        for (value_type key = 1; key <= 100; ++key) {
            if (!check(scalar_scan.query(key) == automatic_scan.query(key),
                       "Fingerprint scan backends disagreed during query")) {
                return 1;
            }
        }

        FingerprintFilter fast_duplicates({8, 8, DeletionMode::fast});
        if (!check(fast_duplicates.insert(77) && fast_duplicates.insert(77) &&
                       fast_duplicates.size() == 2,
                   "Fast mode did not count duplicate insertions as occurrences") ||
            !check(fast_duplicates.erase_fast(77) && fast_duplicates.query(77) &&
                       fast_duplicates.size() == 1,
                   "First fast erase removed all duplicate occurrences") ||
            !check(fast_duplicates.erase_fast(77) && !fast_duplicates.query(77) &&
                       fast_duplicates.size() == 0,
                   "Second fast erase did not remove the final occurrence")) {
            return 1;
        }

        FingerprintFilter exact({16, 1, DeletionMode::exact});
        if (!check(exact.insert(first) && exact.insert(second),
                   "Exact mode failed to insert colliding keys") ||
            !check(exact.insert(first) && exact.size() == 2,
                   "Exact mode duplicate insertion changed set size") ||
            !check(!exact.erase_exact(absent) && exact.size() == 2,
                   "Erasing an absent key changed exact-mode contents") ||
            !check(exact.erase_exact(first) && !exact.query(first) &&
                       exact.query(second) && exact.size() == 1,
                   "Exact erase mishandled a colliding live key") ||
            !check(!exact.erase_exact(first) && exact.size() == 1,
                   "Exact mode erased the same key more than once") ||
            !check(exact.erase_exact(second) && exact.size() == 0,
                   "Exact mode failed to erase the final key")) {
            return 1;
        }

        bool rejected_wrong_mode = false;
        try {
            exact.erase_fast(first);
        } catch (const logic_error&) {
            rejected_wrong_mode = true;
        }
        if (!check(rejected_wrong_mode, "Filter accepted an erase call for the wrong mode") ||
            !test_collision_deletion(DeletionMode::fast) ||
            !test_collision_deletion(DeletionMode::exact) ||
            !test_high_load_churn(DeletionMode::fast) ||
            !test_high_load_churn(DeletionMode::exact)) {
            return 1;
        }

        bool rejected_invalid_fingerprint = false;
        try {
            FingerprintFilter invalid({10, 0, DeletionMode::fast});
        } catch (const invalid_argument&) {
            rejected_invalid_fingerprint = true;
        }
        if (!check(rejected_invalid_fingerprint, "Invalid fingerprint width was accepted")) {
            return 1;
        }

        cout << "[PASS] Fingerprint filter tests passed "
                "(collision chains, deletion, and 95% load churn).\n";
        return 0;
    } catch (const exception& error) {
        cerr << "Unexpected exception: " << error.what() << '\n';
        return 1;
    }
}
