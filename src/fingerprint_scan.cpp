#include "fingerprint_scan.hpp"

#if defined(VQF_HAS_AVX2_SCAN) && defined(_MSC_VER)
#include <intrin.h>
#endif

namespace vqf::detail {

bool avx2_scan_available() noexcept {
#if !defined(VQF_HAS_AVX2_SCAN)
    return false;
#elif defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
    int registers[4]{};
    __cpuid(registers, 1);
    constexpr int osxsave_bit = 1 << 27;
    constexpr int avx_bit = 1 << 28;
    if ((registers[2] & (osxsave_bit | avx_bit)) != (osxsave_bit | avx_bit) ||
        (_xgetbv(0) & 0x6) != 0x6) {
        return false;
    }

    __cpuidex(registers, 7, 0);
    constexpr int avx2_bit = 1 << 5;
    return (registers[1] & avx2_bit) != 0;
#elif defined(__GNUC__) || defined(__clang__)
    __builtin_cpu_init();
    return __builtin_cpu_supports("avx2");
#else
    return false;
#endif
}

#if !defined(VQF_HAS_AVX2_SCAN)
FingerprintProbeResult probe_fingerprint_avx2(
    const std::uint8_t*, const std::uint8_t*, size_type capacity, size_type,
    std::uint64_t, std::uint64_t, unsigned) noexcept {
    return {capacity, capacity};
}
#endif

}  // namespace vqf::detail
