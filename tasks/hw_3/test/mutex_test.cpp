#include "futex_mutex.h"

#include <gtest/gtest.h>

#include <atomic>
#include <latch>
#include <mutex>
#include <thread>
#include <type_traits>
#include <vector>

TEST(FutexMutex, HasBasicApiShape) {
    static_assert(!std::is_copy_constructible_v<FutexMutex>);
    static_assert(!std::is_copy_assignable_v<FutexMutex>);

    [[maybe_unused]] FutexMutex mutex;
    SUCCEED();
}

TEST(FutexMutex, LockUnlockBasicCorrectness) {
    FutexMutex mutex;
    int sharedValue = 0;

    {
        std::lock_guard lock(mutex);
        sharedValue = 42;
    }

    {
        std::lock_guard lock(mutex);
        EXPECT_EQ(sharedValue, 42);
    }

    EXPECT_TRUE(mutex.try_lock());
    EXPECT_FALSE(mutex.try_lock());
    mutex.unlock();
}

TEST(FutexMutex, MutualExclusionPreventsConcurrentCriticalSection) {
    FutexMutex mutex;
    std::latch workersReady{2};
    std::atomic<int> insideCriticalSection{0};
    std::atomic<int> overlapsDetected{0};

    constexpr int kIterations = 50'000;

    std::vector<std::jthread> workers;
    workers.reserve(2);

    for (int i = 0; i < 2; ++i) {
        workers.emplace_back([&] {
            workersReady.count_down();
            workersReady.wait();

            for (int iteration = 0; iteration < kIterations; ++iteration) {
                std::lock_guard lock(mutex);
                const int inside = insideCriticalSection.fetch_add(1, std::memory_order_relaxed) + 1;
                if (inside != 1) {
                    overlapsDetected.fetch_add(1, std::memory_order_relaxed);
                }
                insideCriticalSection.fetch_sub(1, std::memory_order_relaxed);
            }
        });
    }

    workers.clear();

    EXPECT_EQ(overlapsDetected.load(std::memory_order_relaxed), 0);
}

TEST(FutexMutex, LockBlocksUntilUnlocked) {
    FutexMutex mutex;
    mutex.lock();

    std::latch waiterStarted{1};
    std::latch waiterFinished{1};
    std::atomic<bool> acquired{false};

    std::jthread waiter([&] {
        waiterStarted.count_down();

        std::lock_guard lock(mutex);
        acquired.store(true, std::memory_order_release);

        waiterFinished.count_down();
    });

    waiterStarted.wait();

    EXPECT_FALSE(waiterFinished.try_wait());
    EXPECT_FALSE(acquired.load(std::memory_order_acquire));

    mutex.unlock();
    waiterFinished.wait();

    EXPECT_TRUE(acquired.load(std::memory_order_acquire));
}

TEST(FutexMutex, HandlesContentionWithMultipleThreads) {
    FutexMutex mutex;

    constexpr int kThreads = 8;
    constexpr int kIterations = 20'000;

    std::latch workersReady{kThreads};
    std::atomic<int> counter{0};
    std::vector<std::jthread> workers;

    workers.reserve(kThreads);
    for (int i = 0; i < kThreads; ++i) {
        workers.emplace_back([&] {
            workersReady.count_down();
            workersReady.wait();

            for (int iteration = 0; iteration < kIterations; ++iteration) {
                std::lock_guard lock(mutex);
                ++counter;
            }
        });
    }

    workers.clear();

    EXPECT_EQ(counter.load(), kThreads * kIterations);
}

TEST(FutexMutex, StressManyLockUnlockOperations) {
    FutexMutex mutex;

    constexpr int kThreads = 4;
    constexpr int kIterations = 150'000;

    std::latch workersReady{kThreads};
    std::atomic<int> counter{0};
    std::vector<std::jthread> workers;

    workers.reserve(kThreads);
    for (int i = 0; i < kThreads; ++i) {
        workers.emplace_back([&] {
            workersReady.count_down();
            workersReady.wait();

            for (int iteration = 0; iteration < kIterations; ++iteration) {
                mutex.lock();
                ++counter;
                mutex.unlock();
            }
        });
    }

    workers.clear();

    EXPECT_EQ(counter.load(), kThreads * kIterations);
}

TEST(FutexMutex, NoDeadlockInSimpleUsageScenarios) {
    FutexMutex first;
    FutexMutex second;

    constexpr int kIterations = 10'000;

    std::latch workersReady{2};
    std::atomic<int> completed{0};

    std::jthread firstWorker([&] {
        workersReady.count_down();
        workersReady.wait();

        for (int iteration = 0; iteration < kIterations; ++iteration) {
            std::scoped_lock lock(first, second);
        }

        completed.fetch_add(1, std::memory_order_relaxed);
    });

    std::jthread secondWorker([&] {
        workersReady.count_down();
        workersReady.wait();

        for (int iteration = 0; iteration < kIterations; ++iteration) {
            std::scoped_lock lock(first, second);
        }

        completed.fetch_add(1, std::memory_order_relaxed);
    });

    firstWorker.join();
    secondWorker.join();

    EXPECT_EQ(completed.load(std::memory_order_relaxed), 2);
}
