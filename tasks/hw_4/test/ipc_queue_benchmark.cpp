#include "consumerNode.h"
#include "ipc_queue_common.h"
#include "producerNode.h"

#include <benchmark/benchmark.h>

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

std::string UniqueQueueName() {
    static std::atomic<std::uint64_t> counter{0};

    return "/hw4_ipc_bench_" + std::to_string(getpid()) + "_" +
           std::to_string(counter.fetch_add(1, std::memory_order_relaxed));
}

void BM_MpscQueueThroughput(benchmark::State& state) {
    const int producer_threads = static_cast<int>(state.range(0));

    constexpr std::uint32_t kType = 17;
    constexpr std::uint32_t kMessagesPerProducer = 2000;
    const std::string payload(64, 'p');

    const std::string queue_name = UniqueQueueName();
    hw4::ipc::UnlinkQueue(queue_name);

    ProducerNode producer(queue_name, 1ULL << 20);
    ConsumerNode consumer(queue_name);

    for (auto _ : state) {
        std::vector<std::jthread> producers;
        producers.reserve(static_cast<std::size_t>(producer_threads));

        for (int i = 0; i < producer_threads; ++i) {
            producers.emplace_back([&producer, &payload] {
                for (std::uint32_t message = 0; message < kMessagesPerProducer; ++message) {
                    while (!producer.TryPushString(kType, payload)) {
                        std::this_thread::yield();
                    }
                }
            });
        }

        const std::size_t total_messages = static_cast<std::size_t>(producer_threads) * kMessagesPerProducer;

        std::size_t received = 0;
        std::string message;
        while (received < total_messages) {
            if (consumer.TryPopString(kType, message)) {
                ++received;
            } else {
                std::this_thread::yield();
            }
        }

        benchmark::DoNotOptimize(received);
    }

    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(producer_threads) *
                            static_cast<std::int64_t>(kMessagesPerProducer));

    hw4::ipc::UnlinkQueue(queue_name);
}

}

BENCHMARK(BM_MpscQueueThroughput)
    ->Arg(1)
    ->Arg(2)
    ->Arg(4)
    ->Unit(benchmark::kMillisecond);

BENCHMARK_MAIN();

