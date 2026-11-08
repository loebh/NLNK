#include "fingerprint_scan.hpp"

#include <immintrin.h>
#if defined(_MSC_VER)
#include <intrin.h>
#endif

#include <algorithm>
#include <cstring>

namespace vqf::detail {
namespace {

unsigned first_set_bit(unsigned mask) noexcept {
#if defined(_MSC_VER)
    unsigned long index = 0;
    _BitScanForward(&index, mask);
    return static_cast<unsigned>(index);
#else
    return static_cast<unsigned>(__builtin_ctz(mask));
#endif
}

unsigned compare_fingerprint(const std::uint8_t* fingerprints, size_type slot,
                             std::uint64_t low, std::uint64_t high,
                             unsigned bits) noexcept {
    const std::uint8_t* address = fingerprints + slot * (bits / 8);
    const __m256i stored = _mm256_loadu_si256(
        reinterpret_cast<const __m256i*>(address));
    __m256i equal;
    unsigned lanes;
    unsigned bytes_per_lane;

    if (bits == 16) {
        equal = _mm256_cmpeq_epi16(
            stored, _mm256_set1_epi16(static_cast<short>(low)));
        lanes = 16;
        bytes_per_lane = 2;
    } else if (bits == 32) {
        equal = _mm256_cmpeq_epi32(
            stored, _mm256_set1_epi32(static_cast<int>(low)));
        lanes = 8;
        bytes_per_lane = 4;
    } else if (bits == 64) {
        equal = _mm256_cmpeq_epi64(
            stored, _mm256_set1_epi64x(static_cast<long long>(low)));
        lanes = 4;
        bytes_per_lane = 8;
    } else {
        const __m256i expected =
            _mm256_set_epi64x(static_cast<long long>(high),
                              static_cast<long long>(low),
                              static_cast<long long>(high),
                              static_cast<long long>(low));
        equal = _mm256_cmpeq_epi64(stored, expected);
        const unsigned qword_matches = static_cast<unsigned>(
            _mm256_movemask_pd(_mm256_castsi256_pd(equal)));
        return ((qword_matches & 0x3U) == 0x3U ? 1U : 0U) |
               ((qword_matches & 0xcU) == 0xcU ? 2U : 0U);
    }

    const unsigned byte_matches =
        static_cast<unsigned>(_mm256_movemask_epi8(equal));
    unsigned lane_matches = 0;
    for (unsigned lane = 0; lane < lanes; ++lane) {
        lane_matches |= ((byte_matches >> (lane * bytes_per_lane)) & 1U) << lane;
    }
    return lane_matches;
}

void state_masks(const std::uint8_t* states, size_type slot, unsigned lanes,
                 unsigned& occupied, unsigned& empty,
                 unsigned& deleted) noexcept {
    occupied = 0;
    empty = 0;
    deleted = 0;
    for (unsigned lane = 0; lane < lanes; ++lane) {
        const std::uint8_t state = states[slot + lane];
        occupied |= static_cast<unsigned>(state == 1) << lane;
        empty |= static_cast<unsigned>(state == 0) << lane;
        deleted |= static_cast<unsigned>(state == 2) << lane;
    }
}

std::uint64_t load_word(const std::uint8_t* address) noexcept {
    std::uint64_t value = 0;
    std::memcpy(&value, address, sizeof(value));
    return value;
}

bool matches(const std::uint8_t* fingerprints, size_type slot,
             std::uint64_t low, std::uint64_t high, unsigned bits) noexcept {
    const std::uint8_t* address = fingerprints + slot * (bits / 8);
    const std::uint64_t stored_low = load_word(address);
    const std::uint64_t low_mask =
        bits == 64 || bits == 128 ? UINT64_MAX : (UINT64_C(1) << bits) - 1;
    return (stored_low & low_mask) == low &&
           (bits != 128 || load_word(address + sizeof(std::uint64_t)) == high);
}

}  // namespace

FingerprintProbeResult probe_fingerprint_avx2(
    const std::uint8_t* fingerprints, const std::uint8_t* slot_states,
    size_type capacity, size_type start, std::uint64_t low,
    std::uint64_t high, unsigned fingerprint_bits) noexcept {
    const unsigned lanes = 256 / fingerprint_bits;
    size_type first_deleted = capacity;
    size_type slot = start;
    size_type visited = 0;

    for (size_type prefix = 0; prefix < 4 && visited < capacity; ++prefix) {
        const std::uint8_t state = slot_states[slot];
        if (state == 0) {
            return {capacity, first_deleted == capacity ? slot : first_deleted};
        }
        if (state == 2) {
            if (first_deleted == capacity) {
                first_deleted = slot;
            }
        } else if (matches(fingerprints, slot, low, high, fingerprint_bits)) {
            return {slot, capacity};
        }
        ++visited;
        if (++slot == capacity) {
            slot = 0;
        }
    }

    while (visited < capacity) {
        const size_type contiguous = std::min(capacity - slot, capacity - visited);
        if (contiguous < lanes) {
            for (size_type lane = 0; lane < contiguous; ++lane) {
                const size_type current = slot + lane;
                const std::uint8_t state = slot_states[current];
                if (state == 0) {
                    return {capacity, first_deleted == capacity ? current : first_deleted};
                }
                if (state == 2) {
                    if (first_deleted == capacity) {
                        first_deleted = current;
                    }
                } else if (matches(fingerprints, current, low, high,
                                   fingerprint_bits)) {
                    return {current, capacity};
                }
            }
            visited += contiguous;
            slot = 0;
            continue;
        }

        const unsigned fingerprint_matches = compare_fingerprint(
            fingerprints, slot, low, high, fingerprint_bits);
        unsigned occupied;
        unsigned empty;
        unsigned deleted;
        state_masks(slot_states, slot, lanes, occupied, empty, deleted);
        const unsigned candidates = fingerprint_matches & occupied;
        const unsigned first_empty_lane =
            empty == 0 ? lanes : first_set_bit(empty);

        if (candidates != 0) {
            const unsigned candidate_lane = first_set_bit(candidates);
            if (candidate_lane < first_empty_lane) {
                return {slot + candidate_lane, capacity};
            }
        }
        const unsigned before_empty =
            first_empty_lane == lanes ? ((1U << lanes) - 1U)
                                      : ((1U << first_empty_lane) - 1U);
        const unsigned eligible_deleted = deleted & before_empty;
        if (eligible_deleted != 0 && first_deleted == capacity) {
            first_deleted = slot + first_set_bit(eligible_deleted);
        }
        if (empty != 0) {
            const size_type insertion =
                first_deleted == capacity ? slot + first_empty_lane : first_deleted;
            return {capacity, insertion};
        }

        slot += lanes;
        visited += lanes;
        if (slot == capacity) {
            slot = 0;
        }
    }

    return {capacity, first_deleted};
}

}  // namespace vqf::detail
