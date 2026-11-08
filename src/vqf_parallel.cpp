#include "vqf/vqf_parallel.hpp"

#include <cerrno>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef VQF_HAS_NUMA
#include <numa.h>
#include <sched.h>
#endif

#ifdef VQF_HAS_OPENMP
#include <omp.h>
#endif

namespace vqf {

ParallelQueryExecutor::ParallelQueryExecutor(ParallelQueryOptions options)
    : options_(options) {
    if (options_.numa_node < -1) {
        throw std::invalid_argument("NUMA node must be -1 (unbound) or non-negative");
    }
#ifdef VQF_HAS_NUMA
    if (options_.numa_node >= 0 &&
        (numa_available() < 0 || options_.numa_node > numa_max_node())) {
        throw std::invalid_argument("Requested NUMA node is not available");
    }
#endif
#ifndef VQF_HAS_NUMA
    if (options_.numa_node >= 0) {
        throw std::runtime_error(
            "NUMA pinning was requested, but this build has no libnuma support");
    }
#endif
#ifndef VQF_HAS_OPENMP
    if (options_.thread_count > 1) {
        throw std::runtime_error(
            "More than one worker was requested, but this build has no OpenMP support");
    }
#endif
#ifdef VQF_HAS_OPENMP
    if (options_.thread_count >
        static_cast<size_type>(std::numeric_limits<int>::max())) {
        throw std::invalid_argument("Worker count exceeds the OpenMP integer limit");
    }
#endif
}

void ParallelQueryExecutor::batch_process_impl(const void* filter,
                                               QueryFunction query,
                                               const value_type* items,
                                               size_type count,
                                               std::uint8_t* results) const {
    if (count != 0 && (items == nullptr || results == nullptr)) {
        throw std::invalid_argument("batch_process requires non-null buffers");
    }
    if (count == 0) {
        return;
    }

#ifdef VQF_HAS_OPENMP
    const int workers = options_.thread_count == 0
                            ? omp_get_max_threads()
                            : static_cast<int>(options_.thread_count);
#ifdef VQF_HAS_NUMA
    if (options_.numa_node >= 0) {
        std::vector<cpu_set_t> original_affinity(static_cast<size_type>(workers));
        std::vector<unsigned char> have_original(static_cast<size_type>(workers), 0);
        std::vector<int> pin_errors(static_cast<size_type>(workers), 0);
        std::vector<int> restore_errors(static_cast<size_type>(workers), 0);
        bool pin_failed = false;

#pragma omp parallel num_threads(workers) shared(pin_failed)
        {
            const int worker = omp_get_thread_num();
            const size_type index = static_cast<size_type>(worker);
            if (sched_getaffinity(0, sizeof(cpu_set_t), &original_affinity[index]) != 0) {
                pin_errors[index] = errno;
            } else {
                have_original[index] = 1;
                if (numa_run_on_node(options_.numa_node) != 0) {
                    pin_errors[index] = errno;
                }
            }

#pragma omp barrier
#pragma omp single
            {
                for (int error : pin_errors) {
                    if (error != 0) {
                        pin_failed = true;
                        break;
                    }
                }
            }
#pragma omp barrier

            if (!pin_failed) {
#pragma omp for schedule(static)
                for (size_type i = 0; i < count; ++i) {
                    results[i] = static_cast<std::uint8_t>(query(filter, items[i]));
                }
            }

            if (have_original[index] != 0 &&
                sched_setaffinity(0, sizeof(cpu_set_t), &original_affinity[index]) != 0) {
                restore_errors[index] = errno;
            }
        }

        for (size_type i = 0; i < pin_errors.size(); ++i) {
            if (pin_errors[i] != 0) {
                throw std::runtime_error(
                    "Failed to pin worker to NUMA node " +
                    std::to_string(options_.numa_node) + ": " +
                    std::to_string(pin_errors[i]));
            }
            if (restore_errors[i] != 0) {
                throw std::runtime_error(
                    "Failed to restore worker CPU affinity: " +
                    std::to_string(restore_errors[i]));
            }
        }
        return;
    }
#else
    static_cast<void>(options_.numa_node);
#endif

#pragma omp parallel for schedule(static) num_threads(workers)
    for (size_type i = 0; i < count; ++i) {
        results[i] = static_cast<std::uint8_t>(query(filter, items[i]));
    }
#else
#ifdef VQF_HAS_NUMA
    if (options_.numa_node >= 0) {
        cpu_set_t original_affinity;
        if (sched_getaffinity(0, sizeof(cpu_set_t), &original_affinity) != 0) {
            throw std::runtime_error(
                "Failed to read current CPU affinity: " + std::to_string(errno));
        }
        if (numa_run_on_node(options_.numa_node) != 0) {
            const int error = errno;
            if (sched_setaffinity(0, sizeof(cpu_set_t), &original_affinity) != 0) {
                throw std::runtime_error(
                    "NUMA pinning failed and restoring CPU affinity failed: " +
                    std::to_string(errno));
            }
            throw std::runtime_error("Failed to pin to NUMA node " +
                                     std::to_string(options_.numa_node) + ": " +
                                     std::to_string(error));
        }
        for (size_type i = 0; i < count; ++i) {
            results[i] = static_cast<std::uint8_t>(query(filter, items[i]));
        }
        if (sched_setaffinity(0, sizeof(cpu_set_t), &original_affinity) != 0) {
            throw std::runtime_error(
                "Failed to restore CPU affinity: " + std::to_string(errno));
        }
        return;
    }
#endif
    for (size_type i = 0; i < count; ++i) {
        results[i] = static_cast<std::uint8_t>(query(filter, items[i]));
    }
#endif
}

size_type ParallelQueryExecutor::thread_count() const noexcept {
    return options_.thread_count;
}

int ParallelQueryExecutor::numa_node() const noexcept {
    return options_.numa_node;
}

}  // namespace vqf
