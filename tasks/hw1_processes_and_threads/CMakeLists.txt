project(hw1_processes_and_threads_app LANGUAGES CXX)

find_package(Threads REQUIRED)

add_executable(hw1_processes_and_threads_main main.cpp)
target_link_libraries(hw1_processes_and_threads_main PRIVATE Threads::Threads)
