project(hw4_ipc_queue_tests LANGUAGES CXX)
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

add_executable(hw_ipc_test ipc_queue_test.cpp)
target_include_directories(hw_ipc_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(hw_ipc_test PRIVATE Threads::Threads GTest::gtest GTest::gtest_main rt)

include(GoogleTest)
gtest_discover_tests(hw_ipc_test PROPERTIES TIMEOUT 30)

add_executable(hw_ipc_benchmark ipc_queue_benchmark.cpp)
target_include_directories(hw_ipc_benchmark PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(hw_ipc_benchmark PRIVATE Threads::Threads benchmark::benchmark rt)

