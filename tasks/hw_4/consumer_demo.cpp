#include "consumerNode.h"

#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>
#include <thread>

namespace {

constexpr std::uint32_t kImportantType = 1;

}  // namespace

int main(int argc, char** argv) {
    const std::string queue_name = (argc > 1) ? argv[1] : "/hw4_ipc_demo";
    const std::uint32_t expected_type = (argc > 2) ? static_cast<std::uint32_t>(std::stoul(argv[2])) : kImportantType;

    ConsumerNode consumer(queue_name);

    std::size_t received = 0;
    while (received < 20) {
        std::string message;
        if (consumer.TryPopString(expected_type, message)) {
            std::cout << "[consumer] got type=" << expected_type << ": " << message << '\n';
            ++received;
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    std::cout << "[consumer] done\n";
    return 0;
}

