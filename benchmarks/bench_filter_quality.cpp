#include "vqf/vqf_filter.hpp"

#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
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
               performance_frequency();
    }

private:
    static double performance_frequency() {
        static const double frequency = [] {
            LARGE_INTEGER value;
            if (!QueryPerformanceFrequency(&value)) {
                throw runtime_error("QueryPerformanceFrequency failed");
            }
            return static_cast<double>(value.QuadPart);
        }();
        return frequency;
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

double throughput_mops(vqf::size_type operations, double elapsed_seconds) {
    return elapsed_seconds == 0.0
               ? 0.0
               : static_cast<double>(operations) / elapsed_seconds / 1e6;
}

string format_percent(double value) {
    ostringstream output;
    output << fixed << setprecision(3) << value << '%';
    return output.str();
}

void print_table_header() {
    cout << left
         << setw(8) << "Delete"
         << setw(8) << "FP bits"
         << setw(8) << "Scan"
         << setw(8) << "Load"
         << right
         << setw(12) << "FP hits"
         << setw(12) << "FPR"
         << setw(12) << "After"
         << setw(12) << "Insert"
         << setw(12) << "Query +"
         << setw(12) << "Query -"
         << setw(12) << "Churn"
         << '\n'
         << left
         << setw(8) << ""
         << setw(8) << ""
         << setw(8) << ""
         << setw(8) << ""
         << right
         << setw(12) << ""
         << setw(12) << "(%)"
         << setw(12) << "churn (%)"
         << setw(12) << "(Mops/s)"
         << setw(12) << "(Mops/s)"
         << setw(12) << "(Mops/s)"
         << setw(12) << "(Mops/s)"
         << '\n'
         << string(116, '-') << '\n';
}

struct Measurement {
    vqf::size_type capacity;
    vqf::size_type item_count;
    vqf::size_type absent_query_count;
    unsigned fingerprint_bits;
    vqf::DeletionMode mode;
    vqf::FingerprintScanMode scan_mode;
};

bool run_measurement(const Measurement& measurement) {
    using namespace vqf;
    const auto mode_name = measurement.mode == DeletionMode::fast ? "fast" : "exact";
    FingerprintFilter filter(
        {measurement.capacity, measurement.fingerprint_bits, measurement.mode,
         measurement.scan_mode});
    vector<value_type> keys(measurement.item_count);

    const Timer insert_timer;
    for (size_type i = 0; i < measurement.item_count; ++i) {
        keys[i] = static_cast<value_type>(i) + 1;
        if (!filter.insert(keys[i])) {
            cerr << "Insert failed at item " << i << " in " << mode_name
                 << " mode, fingerprint width " << measurement.fingerprint_bits << '\n';
            return false;
        }
    }
    const double insert_seconds = insert_timer.elapsed_seconds();

    size_type positive_hits = 0;
    const Timer positive_timer;
    for (value_type key : keys) {
        positive_hits += static_cast<size_type>(filter.query(key));
    }
    const double positive_seconds = positive_timer.elapsed_seconds();
    if (positive_hits != measurement.item_count) {
        cerr << "False negative detected in " << mode_name
             << " mode, fingerprint width " << measurement.fingerprint_bits << '\n';
        return false;
    }

    size_type false_positives = 0;
    const value_type absent_base = static_cast<value_type>(measurement.capacity) + 1;
    const Timer absent_timer;
    for (size_type i = 0; i < measurement.absent_query_count; ++i) {
        const value_type key = absent_base + static_cast<value_type>(i);
        false_positives += static_cast<size_type>(filter.query(key));
    }
    const double absent_seconds = absent_timer.elapsed_seconds();

    const size_type churn_count = measurement.item_count / 20;
    constexpr size_type churn_cycles = 3;
    const Timer churn_timer;
    value_type next_key = absent_base + measurement.absent_query_count + 1;
    for (size_type cycle = 0; cycle < churn_cycles; ++cycle) {
        for (size_type i = 0; i < churn_count; ++i) {
            const value_type old_key = keys[i];
            const bool erased = measurement.mode == DeletionMode::fast
                                    ? filter.erase_fast(old_key)
                                    : filter.erase_exact(old_key);
            if (!erased) {
                cerr << "Erase failed during churn in " << mode_name << " mode\n";
                return false;
            }
            keys[i] = next_key++;
            if (!filter.insert(keys[i])) {
                cerr << "Reinsert failed during churn in " << mode_name << " mode\n";
                return false;
            }
        }
    }
    const double churn_seconds = churn_timer.elapsed_seconds();

    size_type live_hits = 0;
    for (value_type key : keys) {
        live_hits += static_cast<size_type>(filter.query(key));
    }
    if (live_hits != measurement.item_count || filter.size() != measurement.item_count) {
        cerr << "Live-key membership or item count failed after churn in "
             << mode_name << " mode\n";
        return false;
    }

    size_type post_churn_false_positives = 0;
    for (size_type i = 0; i < measurement.absent_query_count; ++i) {
        const value_type key = absent_base + static_cast<value_type>(i);
        post_churn_false_positives += static_cast<size_type>(filter.query(key));
    }

    const double false_positive_rate =
        measurement.absent_query_count == 0
            ? 0.0
            : static_cast<double>(false_positives) /
                  static_cast<double>(measurement.absent_query_count);
    const double load_percent =
        100.0 * static_cast<double>(measurement.item_count) /
        static_cast<double>(measurement.capacity);

    const double post_churn_fpr =
        measurement.absent_query_count == 0
            ? 0.0
            : 100.0 * static_cast<double>(post_churn_false_positives) /
                  static_cast<double>(measurement.absent_query_count);
    cout << fixed << setprecision(3)
         << left << setw(8) << mode_name
         << setw(8) << measurement.fingerprint_bits
         << setw(8) << (filter.uses_avx2() ? "AVX2" : "scalar")
         << setw(8) << format_percent(load_percent)
         << right
         << setw(12) << false_positives
         << setw(12) << format_percent(false_positive_rate * 100.0)
         << setw(12) << format_percent(post_churn_fpr)
         << setw(12) << throughput_mops(measurement.item_count, insert_seconds)
         << setw(12) << throughput_mops(measurement.item_count, positive_seconds)
         << setw(12) << throughput_mops(measurement.absent_query_count, absent_seconds)
         << setw(12) << throughput_mops(churn_count * churn_cycles * 2, churn_seconds)
         << '\n';
    return true;
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

}  // namespace

int main(int argc, char** argv) {
    using namespace vqf;
    try {
        const size_type capacity =
            argc > 1 ? parse_count(argv[1], "capacity") : size_type{100000};
        const size_type absent_queries =
            argc > 2 ? parse_count(argv[2], "absent query count") : size_type{1000000};
        if (argc > 4) {
            throw invalid_argument(
                "Usage: vqf_bench_filter_quality [capacity] [absent_queries] "
                "[scalar|auto|compare]");
        }
        vector<FingerprintScanMode> scan_modes;
        const string scan_arg = argc > 3 ? argv[3] : "scalar";
        if (scan_arg == "scalar") {
            scan_modes.push_back(FingerprintScanMode::scalar);
        } else if (scan_arg == "auto") {
            scan_modes.push_back(FingerprintScanMode::automatic);
        } else if (scan_arg == "compare") {
            scan_modes.push_back(FingerprintScanMode::scalar);
            scan_modes.push_back(FingerprintScanMode::automatic);
        } else {
            throw invalid_argument("Scan mode must be scalar, auto, or compare");
        }
        if (capacity < 100) {
            throw invalid_argument("Capacity must be at least 100 for 90%/95% load tests");
        }
        const auto maximum_key = numeric_limits<value_type>::max();
        const size_type churn_key_count = capacity / 20 * 3;
        if (capacity > maximum_key - 2 ||
            absent_queries > maximum_key - capacity - 2 ||
            churn_key_count > maximum_key - capacity - absent_queries - 2) {
            throw invalid_argument("Capacity and query count exceed the 64-bit key range");
        }

        const unsigned fingerprint_widths[] = {16, 24, 32};
        const unsigned load_percentages[] = {90, 95};
        const DeletionMode modes[] = {DeletionMode::fast, DeletionMode::exact};

        cout << "Fingerprint-filter benchmark\n"
             << "Capacity: " << capacity << " slots | Absent-key queries: "
             << absent_queries << " per run\n"
             << "FPR = false-positive rate; Query +/- = present/absent keys; "
                "Churn = insert + erase throughput. Scan can be scalar or AVX2.\n\n";
        print_table_header();

        for (unsigned bits : fingerprint_widths) {
            for (unsigned load : load_percentages) {
                const size_type item_count =
                    capacity / 100 * load + (capacity % 100) * load / 100;
                for (DeletionMode mode : modes) {
                    for (FingerprintScanMode scan_mode : scan_modes) {
                        if (!run_measurement({capacity, item_count, absent_queries,
                                              bits, mode, scan_mode})) {
                            return 1;
                        }
                    }
                }
                cout << string(116, '-') << '\n';
            }
        }
        cout << "\nMemory note: fixed table arrays only; exact-key metadata and allocator "
                "overhead are excluded.\n";
        return 0;
    } catch (const exception& error) {
        cerr << error.what() << '\n';
        return 1;
    }
}
