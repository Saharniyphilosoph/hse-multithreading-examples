#include "producerNode.h"

#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>
#include <thread>

namespace {

constexpr std::uint32_t kImportantType = 1;
constexpr std::uint32_t kNoiseType = 2;

}  

int main(int argc, char** argv) {
    const std::string queue_name = (argc > 1) ? argv[1] : "/hw4_ipc_demo";
    const std::size_t queue_capacity = (argc > 2) ? std::stoull(argv[2]) : 1ULL << 16;

    ProducerNode producer(queue_name, queue_capacity);

    for (int i = 0; i < 20; ++i) {
        const std::string important = "important_" + std::to_string(i);
        const std::string noise = "noise_" + std::to_string(i);

        while (!producer.TryPushString(kImportantType, important)) {
            std::this_thread::yield();
        }

        while (!producer.TryPushString(kNoiseType, noise)) {
            std::this_thread::yield();
        }

        std::cout << "[producer] pushed: " << important << " and " << noise << '\n';
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    std::cout << "[producer] done\n";
    return 0;
}

