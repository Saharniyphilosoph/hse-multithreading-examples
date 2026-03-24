#include "process_pool.h"

#include <chrono>
#include <iostream>
#include <thread>

int main() {
    using namespace std::chrono_literals;

    hw5::ProcessPool pool(3);

    auto base = pool.Submit([]() {
        std::this_thread::sleep_for(120ms);
        return 21;
    });

    auto doubled = std::move(base).Then([](int value) { return value * 2; });

    std::cout << "Continuation result: " << doubled.get() << '\n';

    auto to_cancel = pool.Submit([]() {
        std::this_thread::sleep_for(2s);
        return 1;
    });

    std::this_thread::sleep_for(200ms);
    to_cancel.Cancel();

    try {
        std::cout << "Cancelled task result: " << to_cancel.get() << '\n';
    } catch (const std::exception& e) {
        std::cout << "Cancelled task raised exception: " << e.what() << '\n';
    }

    return 0;
}
