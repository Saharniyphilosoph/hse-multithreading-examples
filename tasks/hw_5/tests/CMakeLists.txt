project(hw5_future_process_pool_tests LANGUAGES CXX)

find_package(Threads REQUIRED)

include(FetchContent)

FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.17.0
)
FetchContent_MakeAvailable(googletest)

add_executable(hw5_future_process_pool_test test.cpp)
target_include_directories(hw5_future_process_pool_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(hw5_future_process_pool_test PRIVATE Threads::Threads GTest::gtest GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(hw5_future_process_pool_test PROPERTIES TIMEOUT 40)
