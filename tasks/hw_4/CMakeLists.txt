cmake_minimum_required(VERSION 3.22)

project(hw4_ipc_queue LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)
enable_testing()

add_library(hw_ipc_lib INTERFACE)
target_include_directories(hw_ipc_lib INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(hw_ipc_producer producer_demo.cpp)
target_link_libraries(hw_ipc_producer PRIVATE Threads::Threads hw_ipc_lib rt)

add_executable(hw_ipc_consumer consumer_demo.cpp)
target_link_libraries(hw_ipc_consumer PRIVATE Threads::Threads hw_ipc_lib rt)

add_subdirectory(test)

