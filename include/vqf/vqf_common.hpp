#pragma once
#include <cstddef>
#include <cstdint>
#include <new>

namespace vqf {

using size_type = std::size_t;
using value_type = std::uint64_t;

// Kích thước cache line chuẩn trên kiến trúc x86/ARM (64 bytes)
constexpr size_type CACHE_LINE_SIZE = 64;

// Kiểu dữ liệu hoặc struct được căn chỉnh theo dòng cache line
#if defined(__cpp_lib_hardware_interference_size)
    using std::hardware_destructive_interference_size;
#else
    constexpr std::size_t hardware_destructive_interference_size = CACHE_LINE_SIZE;
#endif

// Bộ đệm cho từng luồng/CPU để tránh hiện tượng False Sharing
template <typename T>
struct alignas(hardware_destructive_interference_size) ThreadWorkerContext {
    T data;
};

} // namespace vqf