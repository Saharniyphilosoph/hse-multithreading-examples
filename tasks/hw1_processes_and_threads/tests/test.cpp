#include "apply_function.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

namespace {

template <class F>
std::chrono::steady_clock::duration MeasureMin(F&& fn, const int attempts = 3) {
    auto best = std::chrono::steady_clock::duration::max();
    for (int i = 0; i < attempts; ++i) {
        const auto start = std::chrono::steady_clock::now();
        fn();
        const auto elapsed = std::chrono::steady_clock::now() - start;
        if (elapsed < best) {
            best = elapsed;
        }
    }
    return best;
}

} 

TEST(ApplyFunction, AppliesTransformSingleThread) {
    std::vector<int> data{1, 2, 3, 4};

    ApplyFunction<int>(data, [](int& value) { value *= 2; }, 1);

    EXPECT_EQ(data, (std::vector<int>{2, 4, 6, 8}));
}

TEST(ApplyFunction, HandlesNonDivisibleThreadPartition) {
    std::vector<int> data{0, 1, 2, 3, 4, 5, 6, 7, 8, 9};

    ApplyFunction<int>(data, [](int& value) { value += 1; }, 3);

    EXPECT_EQ(data, (std::vector<int>{1, 2, 3, 4, 5, 6, 7, 8, 9, 10}));
}

TEST(ApplyFunction, AppliesTransformMultiThreadAndNormalizesTooLargeThreadCount) {
    std::vector<int> data{1, 2, 3, 4, 5};

    ApplyFunction<int>(data, [](int& value) { value += 10; }, 100);

    EXPECT_EQ(data, (std::vector<int>{11, 12, 13, 14, 15}));
}

TEST(ApplyFunction, HandlesEmptyData) {
    std::vector<int> data;
    std::atomic<int> calls = 0;

    ApplyFunction<int>(data, [&calls](int&) { ++calls; }, 4);

    EXPECT_TRUE(data.empty());
    EXPECT_EQ(calls.load(), 0);
}

TEST(ApplyFunction, NormalizesNonPositiveThreadCountToSingleThread) {
    std::vector<int> zeroThreadsData{3, 6, 9};
    std::vector<int> negativeThreadsData{3, 6, 9};

    ApplyFunction<int>(zeroThreadsData, [](int& value) { value /= 3; }, 0);
    ApplyFunction<int>(negativeThreadsData, [](int& value) { value /= 3; }, -5);

    EXPECT_EQ(zeroThreadsData, (std::vector<int>{1, 2, 3}));
    EXPECT_EQ(negativeThreadsData, (std::vector<int>{1, 2, 3}));
}

TEST(ApplyFunction, SingleThreadCanBeFasterForSerializedWork) {
    constexpr int kElementCount = 300000;
    std::vector<int> singleData(kElementCount, 1);
    std::vector<int> multiData(kElementCount, 1);

    std::mutex sharedMutex;
    auto serializedTransform = [&sharedMutex](int& value) {
        std::lock_guard<std::mutex> lock(sharedMutex);
        ++value;
    };

    const auto singleDuration = MeasureMin([&]() {
        ApplyFunction<int>(singleData, serializedTransform, 1);
    });
    const auto multiDuration = MeasureMin([&]() {
        ApplyFunction<int>(multiData, serializedTransform, 8);
    });

    const auto singleUs = std::chrono::duration_cast<std::chrono::microseconds>(singleDuration).count();
    const auto multiUs = std::chrono::duration_cast<std::chrono::microseconds>(multiDuration).count();

    EXPECT_GT(multiUs, singleUs * 12 / 10);
    EXPECT_EQ(singleData, multiData);
}

TEST(ApplyFunction, MultiThreadCanBeFasterForBlockingWork) {
    constexpr int kElementCount = 64;
    std::vector<int> singleData(kElementCount, 0);
    std::vector<int> multiData(kElementCount, 0);

    auto blockingTransform = [](int& value) {
        std::this_thread::sleep_for(std::chrono::milliseconds(3));
        ++value;
    };

    const auto singleDuration = MeasureMin([&]() {
        ApplyFunction<int>(singleData, blockingTransform, 1);
    });
    const auto multiDuration = MeasureMin([&]() {
        ApplyFunction<int>(multiData, blockingTransform, 8);
    });

    const auto singleMs = std::chrono::duration_cast<std::chrono::milliseconds>(singleDuration).count();
    const auto multiMs = std::chrono::duration_cast<std::chrono::milliseconds>(multiDuration).count();

    EXPECT_LT(multiMs + 20, singleMs);
    EXPECT_EQ(singleData, multiData);
}
