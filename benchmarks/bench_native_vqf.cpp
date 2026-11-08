#include "vqf/vqf_native.hpp"
#include "vqf/vqf_tiered_native.hpp"

#include <chrono>
#include <iomanip>
#include <iostream>
#include <stdexcept>
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

double mops(vqf::size_type count, double seconds) {
    return seconds > 0.0 ? static_cast<double>(count) / seconds / 1e6 : 0.0;
}

bool benchmark_load(vqf::size_type requested_slots, unsigned load_percent,
                    vqf::size_type absent_queries) {
    using namespace vqf;
    NativeVQF filter(requested_slots);
    const size_type item_count = filter.capacity() * load_percent / 100;
    vector<value_type> keys(item_count);

    const Timer insert_timer;
    size_type inserted = 0;
    for (; inserted < item_count; ++inserted) {
        keys[inserted] = static_cast<value_type>(inserted) + 1;
        if (!filter.insert(keys[inserted])) {
            break;
        }
    }
    const double insert_seconds = insert_timer.elapsed_seconds();

    size_type positive_hits = 0;
    const Timer positive_timer;
    for (size_type i = 0; i < inserted; ++i) {
        positive_hits += static_cast<size_type>(filter.query(keys[i]));
    }
    const double positive_seconds = positive_timer.elapsed_seconds();
    if (positive_hits != inserted) {
        cerr << "VQF false negative in present-key query phase\n";
        return false;
    }

    const value_type absent_start = static_cast<value_type>(requested_slots) + 1;
    size_type false_positives = 0;
    const Timer absent_timer;
    for (size_type i = 0; i < absent_queries; ++i) {
        false_positives += static_cast<size_type>(
            filter.query(absent_start + static_cast<value_type>(i)));
    }
    const double absent_seconds = absent_timer.elapsed_seconds();

    const size_type churn_count = inserted / 20;
    constexpr size_type churn_cycles = 3;
    value_type next_key = absent_start + static_cast<value_type>(absent_queries) + 1;
    vector<bool> active(inserted, true);
    size_type live_count = inserted;
    size_type churn_operations = 0;
    bool churn_succeeded = true;
    const Timer churn_timer;
    for (size_type cycle = 0; cycle < churn_cycles; ++cycle) {
        for (size_type i = 0; i < churn_count; ++i) {
            if (!active[i]) {
                continue;
            }
            if (!filter.erase(keys[i])) {
                cerr << "VQF erase failed during churn\n";
                return false;
            }
            --live_count;
            ++churn_operations;
            keys[i] = next_key++;
            const bool reinserted = filter.insert(keys[i]);
            ++churn_operations;
            if (!reinserted) {
                active[i] = false;
                churn_succeeded = false;
                break;
            }
            ++live_count;
        }
        if (!churn_succeeded) {
            break;
        }
    }
    const double churn_seconds = churn_timer.elapsed_seconds();

    size_type post_churn_hits = 0;
    for (size_type i = 0; i < inserted; ++i) {
        if (active[i]) {
            post_churn_hits += static_cast<size_type>(filter.query(keys[i]));
        }
    }
    if (post_churn_hits != live_count || filter.size() != live_count) {
        cerr << "VQF live-key check failed after churn\n";
        return false;
    }

    size_type post_churn_false_positives = 0;
    for (size_type i = 0; i < absent_queries; ++i) {
        post_churn_false_positives += static_cast<size_type>(
            filter.query(absent_start + static_cast<value_type>(i)));
    }

    cout << left << setw(8) << (to_string(load_percent) + "%")
         << right << setw(12) << filter.capacity()
         << setw(12) << inserted
         << setw(12) << fixed << setprecision(3)
         << (100.0 * static_cast<double>(inserted) /
             static_cast<double>(filter.capacity()))
         << setw(12) << (100.0 * static_cast<double>(false_positives) /
                         static_cast<double>(absent_queries))
         << setw(12) << (100.0 * static_cast<double>(post_churn_false_positives) /
                         static_cast<double>(absent_queries))
         << setw(12) << mops(inserted, insert_seconds)
         << setw(12) << mops(inserted, positive_seconds)
         << setw(12) << mops(absent_queries, absent_seconds)
         << setw(12) << mops(churn_operations, churn_seconds)
         << setw(14) << filter.memory_bytes()
         << "  " << (churn_succeeded ? "OK" : "reinsert limit")
         << '\n';
    return true;
}

bool benchmark_tiered(vqf::size_type tier_slots,
                      vqf::size_type absent_queries) {
    using namespace vqf;
    TieredNativeVQF filter({tier_slots, 88});
    const size_type base_capacity = filter.capacity();
    const size_type item_count = base_capacity * 95 / 100;
    vector<value_type> keys(item_count);

    const Timer insert_timer;
    for (size_type i = 0; i < item_count; ++i) {
        keys[i] = static_cast<value_type>(i) + 1;
        if (!filter.insert(keys[i])) {
            cerr << "Tiered VQF insertion failed at item " << i << '\n';
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
    if (positive_hits != item_count) {
        cerr << "Tiered VQF false negative before churn\n";
        return false;
    }

    const size_type churn_count = base_capacity / 20;
    value_type next_key = static_cast<value_type>(item_count) + absent_queries + 1;
    const Timer churn_timer;
    for (unsigned cycle = 0; cycle < 3; ++cycle) {
        for (size_type i = 0; i < churn_count; ++i) {
            if (!filter.erase(keys[i])) {
                cerr << "Tiered VQF erase failed during churn\n";
                return false;
            }
            keys[i] = next_key++;
            if (!filter.insert(keys[i])) {
                cerr << "Tiered VQF reinsertion failed during churn\n";
                return false;
            }
        }
    }
    const double churn_seconds = churn_timer.elapsed_seconds();

    size_type false_positives = 0;
    const Timer negative_timer;
    for (size_type i = 0; i < absent_queries; ++i) {
        false_positives += static_cast<size_type>(
            filter.query(next_key + static_cast<value_type>(i)));
    }
    const double negative_seconds = negative_timer.elapsed_seconds();

    positive_hits = 0;
    for (value_type key : keys) {
        positive_hits += static_cast<size_type>(filter.query(key));
    }
    if (positive_hits != item_count || filter.size() != item_count) {
        cerr << "Tiered VQF live-key check failed after churn\n";
        return false;
    }

    cout << "Tiered VQF (88% per-tier ceiling; exact key-to-tier erase index)\n"
         << "Items: " << item_count << " | tiers: " << filter.tier_count()
         << " | allocated slots: " << filter.capacity() << '\n'
         << "FPR: " << fixed << setprecision(3)
         << 100.0 * static_cast<double>(false_positives) /
                static_cast<double>(absent_queries)
         << "% | insert: " << mops(item_count, insert_seconds)
         << " Mops/s | query+: " << mops(item_count, positive_seconds)
         << " Mops/s | query-: " << mops(absent_queries, negative_seconds)
         << " Mops/s | churn: " << mops(churn_count * 6, churn_seconds)
         << " Mops/s | bytes incl. key payload: " << filter.memory_bytes()
         << '\n';
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        if (!vqf::NativeVQF::is_supported()) {
            cerr << "This upstream VQF build requires x86-64 AVX2, BMI, BMI2, and POPCNT.\n"
                    "The benchmark stopped before calling VQF; NativeVQF throws a "
                    "clear exception on unsupported CPUs.\n";
            return 2;
        }

        const vqf::size_type slots =
            argc > 1 ? static_cast<vqf::size_type>(stoull(argv[1]))
                     : vqf::size_type{1000000};
        const vqf::size_type absent_queries =
            argc > 2 ? static_cast<vqf::size_type>(stoull(argv[2]))
                     : vqf::size_type{1000000};
        if (argc > 3 || slots == 0 || absent_queries == 0) {
            throw invalid_argument("Usage: vqf_bench_native [requested_slots] [absent_queries]");
        }

        cout << "Official upstream Vector Quotient Filter (AVX2 implementation)\n"
             << "Requested slots: " << slots << " | absent queries: "
             << absent_queries << " per measurement\n"
             << "FPR measured before and after three insert/erase churn cycles.\n\n"
             << left << setw(8) << "Target"
             << right << setw(12) << "Capacity"
             << setw(12) << "Inserted"
             << setw(12) << "Load %"
             << setw(12) << "FPR %"
             << setw(12) << "After %"
             << setw(12) << "Insert"
             << setw(12) << "Query +"
             << setw(12) << "Query -"
             << setw(12) << "Churn"
             << setw(14) << "Bytes"
             << "  Churn result"
             << "\n"
             << left << setw(8) << ""
             << right << setw(12) << ""
             << setw(12) << ""
             << setw(12) << ""
             << setw(12) << ""
             << setw(12) << ""
             << setw(12) << "(Mops/s)"
             << setw(12) << "(Mops/s)"
             << setw(12) << "(Mops/s)"
             << setw(12) << "(Mops/s)"
             << setw(14) << "(filter)"
             << '\n'
             << string(126, '-') << '\n';

        if (!benchmark_load(slots, 90, absent_queries) ||
            !benchmark_load(slots, 95, absent_queries) ||
            !benchmark_tiered(slots, absent_queries)) {
            return 1;
        }
        return 0;
    } catch (const exception& error) {
        cerr << error.what() << '\n';
        return 1;
    }
}
