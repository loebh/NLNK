cmake_minimum_required(VERSION 3.18)
project(vqf_package_consumer LANGUAGES CXX)

option(VQF_CONSUMER_WITH_NATIVE "Also link the optional native VQF target" OFF)
if(VQF_CONSUMER_WITH_NATIVE)
    find_package(vqf CONFIG REQUIRED COMPONENTS native)
else()
    find_package(vqf CONFIG REQUIRED)
endif()

add_executable(vqf_package_consumer main.cpp)
target_link_libraries(vqf_package_consumer PRIVATE vqf::vqf)

if(VQF_CONSUMER_WITH_NATIVE)
    target_link_libraries(vqf_package_consumer PRIVATE vqf::native)
    target_compile_definitions(vqf_package_consumer PRIVATE VQF_CONSUMER_WITH_NATIVE=1)
endif()
