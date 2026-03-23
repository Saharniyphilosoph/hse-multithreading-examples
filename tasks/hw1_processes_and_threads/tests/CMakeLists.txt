project(hw1_processes_and_threads_tests LANGUAGES CXX)

find_package(Threads REQUIRED)

include(FetchContent)

FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.17.0
)
FetchContent_MakeAvailable(googletest)

set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    googlebenchmark
    GIT_REPOSITORY https://github.com/google/benchmark.git
    GIT_TAG v1.9.5
)
FetchContent_MakeAvailable(googlebenchmark)

add_executable(hw1_processes_and_threads_test test.cpp)
target_include_directories(hw1_processes_and_threads_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(hw1_processes_and_threads_test PRIVATE Threads::Threads GTest::gtest GTest::gtest_main)

add_executable(hw1_processes_and_threads_benchmark benchmark.cpp)
target_include_directories(hw1_processes_and_threads_benchmark PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(hw1_processes_and_threads_benchmark PRIVATE Threads::Threads benchmark::benchmark)
