#include "apply_function.h"

#include <benchmark/benchmark.h>

#include <chrono>
#include <thread>
#include <vector>

namespace {

void BM_TinyWorkloadSingleThread(benchmark::State& state) {
    std::vector<int> base(128, 1);

    for (auto _ : state) {
        std::vector<int> data = base;
        ApplyFunction<int>(data, [](int& value) { value += 1; }, 1);
        benchmark::DoNotOptimize(data);
    }
}

void BM_TinyWorkloadMultiThread(benchmark::State& state) {
    std::vector<int> base(128, 1);

    for (auto _ : state) {
        std::vector<int> data = base;
        ApplyFunction<int>(data, [](int& value) { value += 1; }, 8);
        benchmark::DoNotOptimize(data);
    }
}

void BM_BlockingWorkSingleThread(benchmark::State& state) {
    std::vector<int> base(48, 0);

    for (auto _ : state) {
        std::vector<int> data = base;
        ApplyFunction<int>(data,
                           [](int& value) {
                               std::this_thread::sleep_for(std::chrono::milliseconds(1));
                               ++value;
                           },
                           1);
        benchmark::DoNotOptimize(data);
    }
}

void BM_BlockingWorkMultiThread(benchmark::State& state) {
    std::vector<int> base(48, 0);

    for (auto _ : state) {
        std::vector<int> data = base;
        ApplyFunction<int>(data,
                           [](int& value) {
                               std::this_thread::sleep_for(std::chrono::milliseconds(1));
                               ++value;
                           },
                           8);
        benchmark::DoNotOptimize(data);
    }
}

}  // namespace

BENCHMARK(BM_TinyWorkloadSingleThread);
BENCHMARK(BM_TinyWorkloadMultiThread);
BENCHMARK(BM_BlockingWorkSingleThread);
BENCHMARK(BM_BlockingWorkMultiThread);

BENCHMARK_MAIN();
