add_executable(vqf_test_correctness test_correctness.cpp)
target_link_libraries(vqf_test_correctness PRIVATE vqf::vqf)

add_test(NAME vqf_test_correctness COMMAND vqf_test_correctness)

add_executable(vqf_test_filter test_filter.cpp)
target_link_libraries(vqf_test_filter PRIVATE vqf::vqf)

add_test(NAME vqf_test_filter COMMAND vqf_test_filter)

add_executable(vqf_test_wide_fingerprint test_wide_fingerprint.cpp)
target_link_libraries(vqf_test_wide_fingerprint PRIVATE vqf::vqf)

add_test(NAME vqf_test_wide_fingerprint COMMAND vqf_test_wide_fingerprint)

add_executable(vqf_test_competitor_filters test_competitor_filters.cpp)
target_link_libraries(vqf_test_competitor_filters PRIVATE vqf::vqf)

add_test(NAME vqf_test_competitor_filters COMMAND vqf_test_competitor_filters)

add_executable(vqf_test_parallel test_parallel.cpp)
target_link_libraries(vqf_test_parallel PRIVATE vqf::vqf)

add_test(NAME vqf_test_parallel COMMAND vqf_test_parallel)

if(VQF_ENABLE_UPSTREAM_VQF)
    add_executable(vqf_test_native test_native_vqf.cpp)
    target_link_libraries(vqf_test_native PRIVATE vqf::native vqf::vqf)
    add_test(NAME vqf_test_native COMMAND vqf_test_native)
endif()
