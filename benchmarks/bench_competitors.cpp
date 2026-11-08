#include "vqf/competitor_filters.hpp"

#ifdef VQF_BENCH_HAS_NATIVE
#include "vqf/vqf_native.hpp"
#endif

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

double mops(vqf::size_type operations, double seconds) {
    return seconds > 0.0 ? static_cast<double>(operations) / seconds / 1e6 : 0.0;
}

vqf::size_type percent_of(vqf::size_type count, unsigned percent) {
    return count / 100 * percent + count % 100 * percent / 100;
}

void print_row(const char* name, unsigned target_load, vqf::size_type inserted,
               vqf::size_type physical_capacity,
               vqf::size_type false_positives, vqf::size_type query_count,
               double insert_seconds, double positive_seconds, double negative_seconds,
               double churn_seconds, vqf::size_type churn_operations,
               vqf::size_type memory_bytes, const char* churn_status) {
    const double actual_load =
        physical_capacity == 0
            ? 0.0
            : 100.0 * static_cast<double>(inserted) /
                  static_cast<double>(physical_capacity);
    const double fpr =
        query_count == 0 ? 0.0
                         : 100.0 * static_cast<double>(false_positives) /
                               static_cast<double>(query_count);

    cout << left << setw(18) << name
         << right << setw(8) << (to_string(target_load) + "%")
         << setw(12) << inserted
         << setw(10) << fixed << setprecision(2) << actual_load
         << setw(12) << false_positives
         << setw(10) << setprecision(4) << fpr
         << setw(11) << setprecision(2) << mops(inserted, insert_seconds)
         << setw(11) << mops(inserted, positive_seconds)
         << setw(11) << mops(query_count, negative_seconds)
         << setw(11) << mops(churn_operations, churn_seconds)
         << setw(13) << memory_bytes
         << "  " << churn_status << '\n';
}

template <typename Filter>
bool measure_absent(Filter& filter, vqf::size_type count,
                    vqf::value_type absent_start, vqf::size_type& false_positives,
                    double& seconds) {
    const Timer timer;
    for (vqf::size_type i = 0; i < count; ++i) {
        false_positives += static_cast<vqf::size_type>(
            filter.query(absent_start + static_cast<vqf::value_type>(i)));
    }
    seconds = timer.elapsed_seconds();
    return true;
}

bool benchmark_bloom(vqf::size_type nominal_capacity, unsigned load,
                     vqf::size_type query_count) {
    using namespace vqf;
    constexpr double target_fpr = 0.01;
    const size_type item_count = percent_of(nominal_capacity, load);
    BlockedBloomFilter filter(nominal_capacity, target_fpr);
    vector<value_type> keys(item_count);

    const Timer insert_timer;
    for (size_type i = 0; i < item_count; ++i) {
        keys[i] = static_cast<value_type>(i) + 1;
        filter.insert(keys[i]);
    }
    const double insert_seconds = insert_timer.elapsed_seconds();

    size_type positive_hits = 0;
    const Timer positive_timer;
    for (value_type key : keys) {
        positive_hits += static_cast<size_type>(filter.query(key));
    }
    const double positive_seconds = positive_timer.elapsed_seconds();
    if (positive_hits != item_count) {
        cerr << "Blocked Bloom returned a false negative\n";
        return false;
    }

    size_type false_positives = 0;
    double negative_seconds = 0.0;
    const value_type absent_start =
        static_cast<value_type>(nominal_capacity) + item_count + 1;
    measure_absent(filter, query_count, absent_start, false_positives, negative_seconds);
    print_row("Blocked Bloom", load, item_count, nominal_capacity,
              false_positives, query_count, insert_seconds, positive_seconds,
              negative_seconds, 0.0, 0, filter.memory_bytes(),
              "unsupported (no deletion)");
    return true;
}

bool benchmark_cuckoo(vqf::size_type nominal_capacity, unsigned load,
                      vqf::size_type query_count) {
    using namespace vqf;
    CuckooFilter filter(nominal_capacity);
    const size_type target_items = percent_of(nominal_capacity, load);
    vector<value_type> keys(target_items);

    const Timer insert_timer;
    size_type inserted = 0;
    for (; inserted < target_items; ++inserted) {
        keys[inserted] = static_cast<value_type>(inserted) + 1;
        if (!filter.insert(keys[inserted])) {
            break;
        }
    }
    const double insert_seconds = insert_timer.elapsed_seconds();
    keys.resize(inserted);

    size_type positive_hits = 0;
    const Timer positive_timer;
    for (value_type key : keys) {
        positive_hits += static_cast<size_type>(filter.query(key));
    }
    const double positive_seconds = positive_timer.elapsed_seconds();
    if (positive_hits != inserted || filter.size() != inserted) {
        cerr << "Cuckoo Filter lost an inserted key\n";
        return false;
    }

    size_type false_positives = 0;
    double negative_seconds = 0.0;
    const value_type absent_start =
        static_cast<value_type>(nominal_capacity) + target_items + 1;
    measure_absent(filter, query_count, absent_start, false_positives, negative_seconds);

    const size_type churn_count = inserted / 20;
    vector<value_type> replacements(churn_count * 3);
    for (size_type i = 0; i < replacements.size(); ++i) {
        replacements[i] = absent_start + query_count + static_cast<value_type>(i) + 1;
    }
    vector<bool> active(inserted, true);
    size_type churn_operations = 0;
    bool churn_succeeded = true;
    const Timer churn_timer;
    for (unsigned cycle = 0; cycle < 3 && churn_succeeded; ++cycle) {
        for (size_type i = 0; i < churn_count; ++i) {
            if (!active[i]) {
                continue;
            }
            if (!filter.erase(keys[i])) {
                cerr << "Cuckoo Filter failed to erase a churn key\n";
                return false;
            }
            ++churn_operations;
            const value_type replacement =
                replacements[static_cast<size_type>(cycle) * churn_count + i];
            if (!filter.insert(replacement)) {
                active[i] = false;
                churn_succeeded = false;
                break;
            }
            ++churn_operations;
            keys[i] = replacement;
        }
    }
    const double churn_seconds = churn_timer.elapsed_seconds();

    size_type live_hits = 0;
    for (size_type i = 0; i < keys.size(); ++i) {
        if (active[i]) {
            live_hits += static_cast<size_type>(filter.query(keys[i]));
        }
    }
    if (live_hits != filter.size()) {
        cerr << "Cuckoo Filter membership check failed after churn\n";
        return false;
    }

    print_row("Cuckoo Filter", load, inserted, filter.capacity(),
              false_positives, query_count, insert_seconds, positive_seconds,
              negative_seconds, churn_seconds, churn_operations, filter.memory_bytes(),
              churn_succeeded ? "OK" : "reinsert limit");
    return true;
}

#ifdef VQF_BENCH_HAS_NATIVE
bool benchmark_native(vqf::size_type nominal_capacity, unsigned load,
                      vqf::size_type query_count) {
    using namespace vqf;
    if (!NativeVQF::is_supported()) {
        cout << left << setw(18) << "Upstream VQF" << right << setw(8)
             << (to_string(load) + "%")
             << "  SKIP (requires AVX2, BMI, BMI2, POPCNT)\n";
        return true;
    }

    NativeVQF filter(nominal_capacity);
    const size_type target_items = percent_of(nominal_capacity, load);
    vector<value_type> keys(target_items);
    const Timer insert_timer;
    size_type inserted = 0;
    for (; inserted < target_items; ++inserted) {
        keys[inserted] = static_cast<value_type>(inserted) + 1;
        if (!filter.insert(keys[inserted])) {
            break;
        }
    }
    const double insert_seconds = insert_timer.elapsed_seconds();
    keys.resize(inserted);

    size_type positive_hits = 0;
    const Timer positive_timer;
    for (value_type key : keys) {
        positive_hits += static_cast<size_type>(filter.query(key));
    }
    const double positive_seconds = positive_timer.elapsed_seconds();
    if (positive_hits != inserted || filter.size() != inserted) {
        cerr << "Upstream VQF lost an inserted key\n";
        return false;
    }

    size_type false_positives = 0;
    double negative_seconds = 0.0;
    const value_type absent_start =
        static_cast<value_type>(nominal_capacity) + target_items + 1;
    measure_absent(filter, query_count, absent_start, false_positives, negative_seconds);

    const size_type churn_count = inserted / 20;
    vector<value_type> replacements(churn_count * 3);
    for (size_type i = 0; i < replacements.size(); ++i) {
        replacements[i] = absent_start + query_count + static_cast<value_type>(i) + 1;
    }
    vector<bool> active(inserted, true);
    size_type churn_operations = 0;
    bool churn_succeeded = true;
    const Timer churn_timer;
    for (unsigned cycle = 0; cycle < 3 && churn_succeeded; ++cycle) {
        for (size_type i = 0; i < churn_count; ++i) {
            if (!active[i]) {
                continue;
            }
            if (!filter.erase(keys[i])) {
                cerr << "Upstream VQF failed to erase a churn key\n";
                return false;
            }
            ++churn_operations;
            const value_type replacement =
                replacements[static_cast<size_type>(cycle) * churn_count + i];
            if (!filter.insert(replacement)) {
                active[i] = false;
                churn_succeeded = false;
                break;
            }
            ++churn_operations;
            keys[i] = replacement;
        }
    }
    const double churn_seconds = churn_timer.elapsed_seconds();

    size_type live_hits = 0;
    for (size_type i = 0; i < keys.size(); ++i) {
        if (active[i]) {
            live_hits += static_cast<size_type>(filter.query(keys[i]));
        }
    }
    if (live_hits != filter.size()) {
        cerr << "Upstream VQF membership check failed after churn\n";
        return false;
    }

    print_row("Upstream VQF", load, inserted, filter.capacity(),
              false_positives, query_count, insert_seconds, positive_seconds,
              negative_seconds, churn_seconds, churn_operations, filter.memory_bytes(),
              churn_succeeded ? "OK" : "reinsert limit");
    return true;
}
#endif

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
        const size_type nominal_capacity =
            argc > 1 ? parse_count(argv[1], "nominal capacity") : size_type{100000};
        const size_type query_count =
            argc > 2 ? parse_count(argv[2], "absent query count") : size_type{1000000};
        if (argc > 3 || nominal_capacity < 100) {
            throw invalid_argument(
                "Usage: vqf_bench_competitors [nominal_capacity>=100] [absent_queries]");
        }
        if (nominal_capacity >
                (numeric_limits<value_type>::max() - 10) / 3 ||
            query_count > numeric_limits<value_type>::max() -
                              nominal_capacity * 3 - 10) {
            throw invalid_argument("Capacity and query count exceed the 64-bit key range");
        }

        cout << "Filter competitors: same sequential key workload and item targets\n"
             << "Nominal capacity: " << nominal_capacity
             << " | absent queries: " << query_count << " per filter/load\n"
             << "Bloom target FPR: 1%; its memory is sized for nominal capacity.\n"
             << "Blocked Bloom deletion is unsupported; Cuckoo/VQF churn is tested.\n\n"
             << left << setw(18) << "Filter"
             << right << setw(8) << "Target"
             << setw(12) << "Inserted"
             << setw(10) << "Load %"
             << setw(12) << "FP hits"
             << setw(10) << "FPR %"
             << setw(11) << "Insert"
             << setw(11) << "Query +"
             << setw(11) << "Query -"
             << setw(11) << "Churn"
             << setw(13) << "Bytes"
             << "  Churn result\n"
             << left << setw(18) << ""
             << right << setw(8) << ""
             << setw(12) << ""
             << setw(10) << "(filter)"
             << setw(12) << ""
             << setw(10) << ""
             << setw(11) << "(Mops/s)"
             << setw(11) << "(Mops/s)"
             << setw(11) << "(Mops/s)"
             << setw(11) << "(Mops/s)"
             << setw(13) << "(filter)"
             << '\n'
             << string(142, '-') << '\n';

        for (unsigned load : {90U, 95U}) {
            if (!benchmark_bloom(nominal_capacity, load, query_count) ||
                !benchmark_cuckoo(nominal_capacity, load, query_count)) {
                return 1;
            }
#ifdef VQF_BENCH_HAS_NATIVE
            if (!benchmark_native(nominal_capacity, load, query_count)) {
                return 1;
            }
#endif
            cout << string(142, '-') << '\n';
        }
        cout << "\nMops/s = million operations per second. Bloom load is inserted / "
                "expected item capacity; Cuckoo/VQF load is inserted / allocated "
                "slots. These percentages are not equivalent. Bloom FPR depends on "
                "fill relative to its nominal-capacity bit budget.\n";
        return 0;
    } catch (const exception& error) {
        cerr << error.what() << '\n';
        return 1;
    }
}
