cmake_minimum_required(VERSION 3.18)

project(vqf_optimized VERSION 0.1.0 LANGUAGES CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

option(VQF_BUILD_TESTS "Build VQF tests" ON)
option(VQF_BUILD_BENCHMARKS "Build the standalone throughput benchmark" OFF)
option(VQF_ENABLE_UPSTREAM_VQF "Build the upstream AVX2 Vector Quotient Filter" OFF)
option(VQF_ENABLE_AVX2_SCAN "Build the runtime-dispatched AVX2 fingerprint probe kernel" ON)
option(VQF_ENABLE_AVX2 "Compile the entire library for AVX2 (requires AVX2 at runtime)" OFF)
option(VQF_ENABLE_AVX512 "Compile the entire library for AVX-512 (requires AVX-512 at runtime)" OFF)
option(VQF_ENABLE_OPENMP "Enable OpenMP when available" ON)
option(VQF_ENABLE_NUMA "Enable NUMA support when available" ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(CheckHardwareSupport)

add_library(vqf
    src/vqf_core.cpp
    src/vqf_filter.cpp
    src/fingerprint_scan.cpp
    src/vqf_parallel.cpp
    src/wide_fingerprint.cpp
    src/competitor_filters.cpp
)
add_library(vqf::vqf ALIAS vqf)

target_compile_features(vqf PUBLIC cxx_std_17)
target_include_directories(vqf
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

if(MSVC)
    target_compile_options(vqf PRIVATE /W4 /permissive-)
else()
    target_compile_options(vqf PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(VQF_ENABLE_AVX2_SCAN AND
   CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64|i.86|x86)$" AND
   VQF_COMPILER_HAS_AVX2)
    target_sources(vqf PRIVATE src/fingerprint_scan_avx2.cpp)
    if(MSVC)
        set_source_files_properties(src/fingerprint_scan_avx2.cpp
            PROPERTIES COMPILE_OPTIONS "/arch:AVX2")
    else()
        set_source_files_properties(src/fingerprint_scan_avx2.cpp
            PROPERTIES COMPILE_OPTIONS "-mavx2")
    endif()
    target_compile_definitions(vqf PRIVATE VQF_HAS_AVX2_SCAN=1)
endif()

if(VQF_ENABLE_AVX2)
    if(NOT VQF_COMPILER_HAS_AVX2)
        message(FATAL_ERROR "VQF_ENABLE_AVX2 is ON, but the compiler does not support AVX2 flags.")
    endif()
    if(MSVC)
        target_compile_options(vqf PRIVATE /arch:AVX2)
    else()
        target_compile_options(vqf PRIVATE -mavx2)
    endif()
    target_compile_definitions(vqf PUBLIC VQF_COMPILED_WITH_AVX2=1)
endif()

if(VQF_ENABLE_AVX512)
    if(NOT VQF_COMPILER_HAS_AVX512)
        message(FATAL_ERROR "VQF_ENABLE_AVX512 is ON, but the compiler does not support AVX-512 flags.")
    endif()
    if(MSVC)
        target_compile_options(vqf PRIVATE /arch:AVX512)
    else()
        target_compile_options(vqf PRIVATE -mavx512f)
    endif()
    target_compile_definitions(vqf PUBLIC VQF_COMPILED_WITH_AVX512=1)
endif()

if(VQF_ENABLE_OPENMP AND OpenMP_CXX_FOUND)
    target_link_libraries(vqf PUBLIC OpenMP::OpenMP_CXX)
    target_compile_definitions(vqf PUBLIC VQF_HAS_OPENMP=1)
endif()

if(VQF_ENABLE_NUMA AND CMAKE_SYSTEM_NAME STREQUAL "Linux" AND
   VQF_NUMA_INCLUDE_DIR AND VQF_NUMA_LIBRARY)
    add_library(vqf_numa UNKNOWN IMPORTED)
    set_target_properties(vqf_numa PROPERTIES
        IMPORTED_LOCATION "${VQF_NUMA_LIBRARY}"
    )
    target_include_directories(vqf PRIVATE "${VQF_NUMA_INCLUDE_DIR}")
    target_link_libraries(vqf PUBLIC vqf_numa)
    target_compile_definitions(vqf PUBLIC VQF_HAS_NUMA=1)
endif()

if(VQF_ENABLE_UPSTREAM_VQF)
    if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
        message(FATAL_ERROR "The upstream VQF implementation requires a 64-bit target.")
    endif()
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        message(FATAL_ERROR "The upstream VQF implementation currently requires GCC or Clang.")
    endif()
    if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
        message(FATAL_ERROR "The upstream VQF implementation requires an x86-64 target.")
    endif()

    include(CheckCXXCompilerFlag)
    foreach(flag IN ITEMS -mavx2 -mbmi -mbmi2 -mpopcnt)
        string(MAKE_C_IDENTIFIER "${flag}" flag_id)
        check_cxx_compiler_flag("${flag}" "VQF_SUPPORTS_${flag_id}")
        if(NOT VQF_SUPPORTS_${flag_id})
            message(FATAL_ERROR "The compiler does not support required VQF flag: ${flag}")
        endif()
    endforeach()

    set(VQF_UPSTREAM_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/vqf")
    set_source_files_properties(
        "${VQF_UPSTREAM_DIR}/src/vqf_filter.c"
        "${VQF_UPSTREAM_DIR}/src/shuffle_matrix_256.c"
        PROPERTIES LANGUAGE CXX
    )
    add_library(vqf_upstream STATIC
        "${VQF_UPSTREAM_DIR}/src/vqf_filter.c"
        "${VQF_UPSTREAM_DIR}/src/shuffle_matrix_256.c"
    )
    add_library(vqf::upstream ALIAS vqf_upstream)
    target_include_directories(vqf_upstream PRIVATE "${VQF_UPSTREAM_DIR}/include")
    target_compile_options(vqf_upstream PRIVATE -mavx2 -mbmi -mbmi2 -mpopcnt)
    target_compile_options(vqf_upstream PRIVATE -w)

    add_library(vqf_native
        src/vqf_native.cpp
        src/vqf_tiered_native.cpp
    )
    add_library(vqf::native ALIAS vqf_native)
    target_compile_features(vqf_native PUBLIC cxx_std_17)
    target_include_directories(vqf_native
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
        PRIVATE
            "${VQF_UPSTREAM_DIR}/include"
    )
    target_link_libraries(vqf_native PUBLIC vqf::vqf vqf::upstream)
endif()

if(VQF_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(VQF_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()

set_target_properties(vqf PROPERTIES EXPORT_NAME vqf)
set(VQF_INSTALL_TARGETS vqf)
set(VQF_PACKAGE_HAS_NATIVE OFF)
if(VQF_ENABLE_UPSTREAM_VQF)
    set_target_properties(vqf_upstream PROPERTIES EXPORT_NAME upstream)
    set_target_properties(vqf_native PROPERTIES EXPORT_NAME native)
    list(APPEND VQF_INSTALL_TARGETS vqf_upstream vqf_native)
    set(VQF_PACKAGE_HAS_NATIVE ON)
endif()

set(VQF_PACKAGE_HAS_OPENMP OFF)
if(VQF_ENABLE_OPENMP AND OpenMP_CXX_FOUND)
    set(VQF_PACKAGE_HAS_OPENMP ON)
endif()

set(VQF_PACKAGE_HAS_NUMA OFF)
if(VQF_ENABLE_NUMA AND CMAKE_SYSTEM_NAME STREQUAL "Linux" AND
   VQF_NUMA_INCLUDE_DIR AND VQF_NUMA_LIBRARY)
    set(VQF_PACKAGE_HAS_NUMA ON)
endif()

install(
    TARGETS ${VQF_INSTALL_TARGETS}
    EXPORT vqfTargets
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
)
install(
    DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/include/vqf"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
    FILES_MATCHING PATTERN "*.hpp"
)
install(
    EXPORT vqfTargets
    FILE vqfTargets.cmake
    NAMESPACE vqf::
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/vqf"
)

configure_package_config_file(
    cmake/vqfConfig.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/vqfConfig.cmake"
    INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/vqf"
)
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/vqfConfigVersion.cmake"
    VERSION "${PROJECT_VERSION}"
    COMPATIBILITY SameMajorVersion
)
install(
    FILES
        "${CMAKE_CURRENT_BINARY_DIR}/vqfConfig.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/vqfConfigVersion.cmake"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/vqf"
)
if(VQF_ENABLE_UPSTREAM_VQF)
    install(
        FILES "${CMAKE_CURRENT_SOURCE_DIR}/third_party/vqf/LICENSE"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/vqf"
        RENAME LICENSE-upstream-vqf.txt
    )
endif()
