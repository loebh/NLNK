#include "vqf/wide_fingerprint.hpp"

#include <algorithm>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

using namespace std;

namespace {

class Timer {
public:
#if defined(_WIN32)
    Timer() {
        if (!QueryPerformanceCounter(&start_)) {
            throw runtime_error("QueryPerformanceCounter failed");
        }
    }

    double elapsed_seconds() const {
        LARGE_INTEGER current;
        if (!QueryPerformanceCounter(&current)) {
            throw runtime_error("QueryPerformanceCounter failed");
        }
        return static_cast<double>(current.QuadPart - start_.QuadPart) /
               frequency();
    }

private:
    static double frequency() {
        static const double value = [] {
            LARGE_INTEGER result;
            if (!QueryPerformanceFrequency(&result)) {
                throw runtime_error("QueryPerformanceFrequency failed");
            }
            return static_cast<double>(result.QuadPart);
        }();
        return value;
    }

    LARGE_INTEGER start_{};
#else
    Timer() : start_(chrono::steady_clock::now()) {}

    double elapsed_seconds() const {
        return chrono::duration<double>(chrono::steady_clock::now() - start_).count();
    }

private:
    chrono::steady_clock::time_point start_;
#endif
};

std::uint64_t mix(std::uint64_t value) noexcept {
    value ^= value >> 30;
    value *= UINT64_C(0xbf58476d1ce4e5b9);
    value ^= value >> 27;
    value *= UINT64_C(0x94d049bb133111eb);
    value ^= value >> 31;
    return value;
}

vqf::WideKey make_key(std::uint64_t value, unsigned key_bits) {
    using namespace vqf;
    WideKey key{};
    key[0] = value;
    if (key_bits > 64) {
        key[1] = mix(value ^ UINT64_C(0x9e3779b97f4a7c15));
    }
    if (key_bits > 128) {
        key[2] = mix(value ^ UINT64_C(0xd1b54a32d192ed03));
    }
    if (key_bits > 192) {
        key[3] = mix(value ^ UINT64_C(0x94d049bb133111eb));
    }
    if (key_bits < 64) {
        key[0] &= (uint64_t{1} << key_bits) - 1;
    }
    return key;
}

double throughput_mops(vqf::size_type operations, double seconds) {
    return seconds > 0.0 ? static_cast<double>(operations) / seconds / 1e6 : 0.0;
}

vqf::size_type parse_count(const char* text, const char* name) {
    try {
        const unsigned long long parsed = stoull(text);
        if (parsed == 0 || parsed > numeric_limits<vqf::size_type>::max()) {
            throw out_of_range(name);
        }
        return static_cast<vqf::size_type>(parsed);
    } catch (const exception&) {
        throw invalid_argument(string("Invalid ") + name + ": " + text);
    }
}

struct Measurement {
    vqf::size_type capacity;
    vqf::size_type item_count;
    vqf::size_type absent_query_count;
    vqf::size_type absent_key_count;
    unsigned key_bits;
    unsigned fingerprint_bits;
    unsigned load_percent;
    vqf::DeletionMode mode;
    vqf::FingerprintScanMode scan_mode;
};

bool run_measurement(const Measurement& measurement) {
    using namespace vqf;
    const char* mode_name = measurement.mode == DeletionMode::fast ? "fast" : "exact";
    WideFingerprintFilter filter(
        {measurement.capacity, measurement.fingerprint_bits, measurement.mode,
         measurement.scan_mode});
    vector<WideKey> keys(measurement.item_count);
    for (size_type i = 0; i < measurement.item_count; ++i) {
        keys[i] = make_key(static_cast<uint64_t>(i), measurement.key_bits);
    }

    const Timer insert_timer;
    for (const WideKey& key : keys) {
        if (!filter.insert(key)) {
            cerr << "Insert failed for " << measurement.key_bits << "-bit keys, "
                 << measurement.fingerprint_bits << "-bit fingerprints, "
                 << mode_name << " deletion\n";
            return false;
        }
    }
    const double insert_seconds = insert_timer.elapsed_seconds();

    size_type positive_hits = 0;
    const Timer positive_timer;
    for (const WideKey& key : keys) {
        positive_hits += static_cast<size_type>(filter.query(key));
    }
    const double positive_seconds = positive_timer.elapsed_seconds();
    if (positive_hits != measurement.item_count) {
        cerr << "False negative for " << measurement.key_bits << "-bit keys, "
             << measurement.fingerprint_bits << "-bit fingerprints\n";
        return false;
    }

    const uint64_t churn_count = measurement.item_count / 20;
    const uint64_t churn_key_count = churn_count * 3;
    const uint64_t absent_start =
        static_cast<uint64_t>(measurement.item_count) + churn_key_count;
    vector<WideKey> churn_keys(static_cast<size_type>(churn_key_count));
    for (size_type i = 0; i < churn_keys.size(); ++i) {
        churn_keys[i] = make_key(static_cast<uint64_t>(measurement.item_count) + i,
                                 measurement.key_bits);
    }
    vector<WideKey> absent_keys;
    absent_keys.reserve(measurement.absent_query_count);
    for (size_type i = 0; i < measurement.absent_query_count; ++i) {
        const uint64_t key_id = absent_start + i % measurement.absent_key_count;
        absent_keys.push_back(make_key(key_id, measurement.key_bits));
    }

    size_type false_positives = 0;
    const Timer absent_timer;
    for (const WideKey& key : absent_keys) {
        false_positives += static_cast<size_type>(filter.query(key));
    }
    const double absent_seconds = absent_timer.elapsed_seconds();

    const Timer churn_timer;
    for (unsigned cycle = 0; cycle < 3; ++cycle) {
        for (size_type i = 0; i < churn_count; ++i) {
            const bool erased = measurement.mode == DeletionMode::fast
                                    ? filter.erase_fast(keys[i])
                                    : filter.erase_exact(keys[i]);
            if (!erased) {
                cerr << "Erase failed during churn for " << measurement.key_bits
                     << "-bit keys\n";
                return false;
            }
            keys[i] = churn_keys[static_cast<size_type>(cycle) * churn_count + i];
            if (!filter.insert(keys[i])) {
                cerr << "Reinsert failed during churn for " << measurement.key_bits
                     << "-bit keys\n";
                return false;
            }
        }
    }
    const double churn_seconds = churn_timer.elapsed_seconds();

    size_type live_hits = 0;
    for (const WideKey& key : keys) {
        live_hits += static_cast<size_type>(filter.query(key));
    }
    if (live_hits != measurement.item_count ||
        filter.size() != measurement.item_count) {
        cerr << "Live-key membership failed after churn\n";
        return false;
    }

    size_type post_churn_false_positives = 0;
    for (const WideKey& key : absent_keys) {
        post_churn_false_positives += static_cast<size_type>(filter.query(key));
    }

    const double fpr =
        100.0 * static_cast<double>(false_positives) /
        static_cast<double>(absent_keys.size());
    const double post_churn_fpr =
        100.0 * static_cast<double>(post_churn_false_positives) /
        static_cast<double>(absent_keys.size());
    const size_type exact_payload = filter.exact_key_payload_bytes();

    cout << left << setw(6) << measurement.key_bits
         << setw(6) << measurement.fingerprint_bits
         << setw(8) << mode_name
         << setw(8) << (filter.uses_avx2() ? "AVX2" : "scalar")
         << right << setw(6) << measurement.load_percent
         << setw(11) << false_positives
         << setw(10) << fixed << setprecision(3) << fpr
         << setw(10) << post_churn_fpr
         << setw(11) << throughput_mops(measurement.item_count, insert_seconds)
         << setw(11) << throughput_mops(measurement.item_count, positive_seconds)
         << setw(11) << throughput_mops(absent_keys.size(), absent_seconds)
         << setw(11) << throughput_mops(churn_key_count * 2, churn_seconds)
         << setw(12) << filter.fingerprint_memory_bytes()
         << setw(14) << exact_payload
         << '\n';
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    using namespace vqf;
    try {
        const size_type requested_capacity =
            argc > 1 ? parse_count(argv[1], "capacity") : size_type{100000};
        const size_type absent_queries =
            argc > 2 ? parse_count(argv[2], "absent query count") : size_type{1000000};
        if (argc > 4 || requested_capacity < 100) {
            throw invalid_argument(
                "Usage: vqf_bench_wide_fingerprint [capacity>=100] "
                "[absent_queries] [auto|scalar|compare]");
        }

        vector<FingerprintScanMode> scan_modes;
        const string scan_arg = argc > 3 ? argv[3] : "auto";
        if (scan_arg == "auto") {
            scan_modes.push_back(FingerprintScanMode::automatic);
        } else if (scan_arg == "scalar") {
            scan_modes.push_back(FingerprintScanMode::scalar);
        } else if (scan_arg == "compare") {
            scan_modes.push_back(FingerprintScanMode::scalar);
            scan_modes.push_back(FingerprintScanMode::automatic);
        } else {
            throw invalid_argument("Scan mode must be auto, scalar, or compare");
        }

        const unsigned key_widths[] = {16, 31, 64, 128, 256};
        const unsigned fingerprint_widths[] = {16, 32, 64, 128};
        const unsigned load_percentages[] = {90, 95};
        const DeletionMode modes[] = {DeletionMode::fast, DeletionMode::exact};

        cout << "Wide-key / packed-fingerprint prototype benchmark\n"
             << "Requested capacity: " << requested_capacity
             << " | absent queries: " << absent_queries
             << " per measurement\n"
             << "Key width is input representation; fingerprint width is stored bits.\n"
             << "Key-to-fingerprint hashing is included; key/query generation is excluded.\n"
             << "VQF upstream is measured separately (fixed 8-bit tag).\n\n"
             << left << setw(6) << "Key"
             << setw(6) << "FP"
             << setw(8) << "Delete"
             << setw(8) << "Scan"
             << right << setw(6) << "Load"
             << setw(11) << "FP hits"
             << setw(10) << "FPR %"
             << setw(10) << "After %"
             << setw(11) << "Insert"
             << setw(11) << "Query +"
             << setw(11) << "Query -"
             << setw(11) << "Churn"
             << setw(12) << "FP bytes"
             << setw(14) << "Exact bytes"
             << "\n"
             << left << setw(6) << "(bits)"
             << setw(6) << "(bits)"
             << setw(8) << ""
             << setw(8) << ""
             << right << setw(6) << "(%)"
             << setw(11) << ""
             << setw(10) << ""
             << setw(10) << ""
             << setw(11) << "(Mops/s)"
             << setw(11) << "(Mops/s)"
             << setw(11) << "(Mops/s)"
             << setw(11) << "(Mops/s)"
             << setw(12) << "(arrays)"
             << setw(14) << "(key payload)"
             << '\n'
             << string(128, '-') << '\n';

        for (unsigned key_bits : key_widths) {
            size_type capacity = requested_capacity;
            size_type absent_key_count = numeric_limits<size_type>::max();
            if (key_bits < 64) {
                const size_type key_domain = size_type{1} << key_bits;
                capacity = min(capacity, key_domain * 90 / 100);
                const size_type maximum_items = capacity * 95 / 100;
                const size_type maximum_churn_keys = (maximum_items / 20) * 3;
                absent_key_count = key_domain - maximum_items - maximum_churn_keys;
                if (absent_key_count == 0) {
                    throw invalid_argument("Capacity leaves no keys for absent queries");
                }
                if (key_bits == 16) {
                    cout << "16-bit input domain limits capacity to " << capacity
                         << "; absent 16-bit keys are sampled with replacement.\n";
                }
            }

            for (unsigned load : load_percentages) {
                const size_type item_count = capacity * load / 100;
                size_type unique_absent_keys = absent_key_count;
                if (key_bits >= 64) {
                    const size_type churn_keys = (item_count / 20) * 3;
                    unique_absent_keys = numeric_limits<size_type>::max() -
                                         item_count - churn_keys;
                } else {
                    unique_absent_keys = (size_type{1} << key_bits) - item_count -
                                         (item_count / 20) * 3;
                }
                for (unsigned fp_bits : fingerprint_widths) {
                    for (DeletionMode mode : modes) {
                        for (FingerprintScanMode scan_mode : scan_modes) {
                            if (!run_measurement({capacity, item_count, absent_queries,
                                                  unique_absent_keys, key_bits, fp_bits,
                                                  load, mode, scan_mode})) {
                                return 1;
                            }
                        }
                    }
                }
                cout << string(128, '-') << '\n';
            }
        }
        cout << "\nFP bytes include packed fingerprint, reference-count, and slot-state arrays. "
                "Exact bytes are key payload only; unordered_set bucket/node allocator "
                "overhead is excluded.\n";
        return 0;
    } catch (const exception& error) {
        cerr << error.what() << '\n';
        return 1;
    }
}
