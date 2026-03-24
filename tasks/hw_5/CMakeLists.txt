cmake_minimum_required(VERSION 3.22)

project(hw5_future_process_pool LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)
enable_testing()

add_executable(hw5_future_process_pool_main main.cpp)
target_include_directories(hw5_future_process_pool_main PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(hw5_future_process_pool_main PRIVATE Threads::Threads)

add_subdirectory(tests)
