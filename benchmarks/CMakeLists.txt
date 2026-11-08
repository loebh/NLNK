add_executable(vqf_bench_throughput bench_throughput.cpp)
target_link_libraries(vqf_bench_throughput PRIVATE vqf::vqf)

add_executable(vqf_bench_parallel bench_parallel.cpp)
target_link_libraries(vqf_bench_parallel PRIVATE vqf::vqf)

add_executable(vqf_bench_filter_quality bench_filter_quality.cpp)
target_link_libraries(vqf_bench_filter_quality PRIVATE vqf::vqf)

add_executable(vqf_bench_wide_fingerprint bench_wide_fingerprint.cpp)
target_link_libraries(vqf_bench_wide_fingerprint PRIVATE vqf::vqf)

add_executable(vqf_bench_competitors bench_competitors.cpp)
target_link_libraries(vqf_bench_competitors PRIVATE vqf::vqf)

if(VQF_ENABLE_UPSTREAM_VQF)
    add_executable(vqf_bench_native bench_native_vqf.cpp)
    target_link_libraries(vqf_bench_native PRIVATE vqf::native)
    target_link_libraries(vqf_bench_competitors PRIVATE vqf::native)
    target_compile_definitions(vqf_bench_competitors PRIVATE VQF_BENCH_HAS_NATIVE=1)
endif()
