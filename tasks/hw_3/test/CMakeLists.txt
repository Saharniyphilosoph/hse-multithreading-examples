project(hw3_futex_mutex_tests LANGUAGES CXX)

find_package(Threads REQUIRED)

include(FetchContent)

FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.17.0
)
FetchContent_MakeAvailable(googletest)

add_executable(hw3_futex_mutex_test mutex_test.cpp)
target_include_directories(hw3_futex_mutex_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(hw3_futex_mutex_test PRIVATE Threads::Threads GTest::gtest GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(hw3_futex_mutex_test PROPERTIES TIMEOUT 20)
