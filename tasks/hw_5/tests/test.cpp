#include "process_pool.h"

#include <chrono>
#include <set>
#include <thread>

#include <sys/types.h>
#include <unistd.h>

#include <gtest/gtest.h>

namespace {

TEST(ProcessPool, SubmitReturnsValue) {
    hw5::ProcessPool pool(2);

    auto future = pool.Submit([]() { return 7 * 6; });

    EXPECT_EQ(future.get(), 42);
}

TEST(ProcessPool, ExceptionPropagatesToFuture) {
    hw5::ProcessPool pool(2);

    auto future = pool.Submit([]() -> int {
        throw std::runtime_error("boom");
    });

    EXPECT_THROW(
        {
            try {
                static_cast<void>(future.get());
            } catch (const std::runtime_error& e) {
                EXPECT_STREQ(e.what(), "boom");
                throw;
            }
        },
        std::runtime_error);
}

TEST(ProcessPool, ContinuationWorks) {
    hw5::ProcessPool pool(2);

    auto first = pool.Submit([]() { return 10; });
    auto second = std::move(first).Then([](int value) { return value + 5; });

    EXPECT_EQ(second.get(), 15);
}

TEST(ProcessPool, CanCancelTaskBeforeStart) {
    using namespace std::chrono_literals;

    hw5::ProcessPool pool(1);

    auto blocker = pool.Submit([]() {
        std::this_thread::sleep_for(300ms);
        return 1;
    });

    auto cancelled = pool.Submit([]() {
        std::this_thread::sleep_for(100ms);
        return 2;
    });

    EXPECT_TRUE(cancelled.Cancel());
    EXPECT_EQ(blocker.get(), 1);

    EXPECT_THROW(
        {
            try {
                static_cast<void>(cancelled.get());
            } catch (const hw5::FutureCancelled& e) {
                EXPECT_NE(std::string(e.what()).find("cancelled"), std::string::npos);
                throw;
            }
        },
        hw5::FutureCancelled);
}

TEST(ProcessPool, CanCancelRunningTask) {
    using namespace std::chrono_literals;

    hw5::ProcessPool pool(1);

    auto running = pool.Submit([]() {
        std::this_thread::sleep_for(400ms);
        return 5;
    });

    std::this_thread::sleep_for(80ms);
    EXPECT_TRUE(running.Cancel());

    EXPECT_THROW(
        {
            try {
                static_cast<void>(running.get());
            } catch (const hw5::FutureCancelled& e) {
                EXPECT_NE(std::string(e.what()).find("cancelled"), std::string::npos);
                throw;
            }
        },
        hw5::FutureCancelled);
}

TEST(ProcessPool, ReusesWorkerProcesses) {
    using namespace std::chrono_literals;

    hw5::ProcessPool pool(2);

    std::vector<hw5::Future<pid_t>> futures;
    futures.reserve(12);

    for (int i = 0; i < 12; ++i) {
        futures.emplace_back(pool.Submit([]() -> pid_t {
            std::this_thread::sleep_for(20ms);
            return getpid();
        }));
    }

    std::set<pid_t> worker_pids;
    for (auto& future : futures) {
        worker_pids.insert(future.get());
    }

    EXPECT_LE(worker_pids.size(), 2u);
    EXPECT_GE(worker_pids.size(), 1u);
}

TEST(ProcessPool, RunsTasksInParallelByTime) {
    using namespace std::chrono_literals;

    hw5::ProcessPool pool(4);

    const auto start = std::chrono::steady_clock::now();

    auto f1 = pool.Submit([]() {
        std::this_thread::sleep_for(220ms);
        return 1;
    });
    auto f2 = pool.Submit([]() {
        std::this_thread::sleep_for(220ms);
        return 2;
    });
    auto f3 = pool.Submit([]() {
        std::this_thread::sleep_for(220ms);
        return 3;
    });
    auto f4 = pool.Submit([]() {
        std::this_thread::sleep_for(220ms);
        return 4;
    });

    EXPECT_EQ(f1.get(), 1);
    EXPECT_EQ(f2.get(), 2);
    EXPECT_EQ(f3.get(), 3);
    EXPECT_EQ(f4.get(), 4);

    const auto elapsed = std::chrono::steady_clock::now() - start;
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();

    EXPECT_LT(ms, 650);
}

TEST(ProcessPool, DestructorWaitsForSubmittedTasks) {
    using namespace std::chrono_literals;

    const auto start = std::chrono::steady_clock::now();
    {
        hw5::ProcessPool pool(1);
        auto future = pool.Submit([]() {
            std::this_thread::sleep_for(260ms);
            return 1;
        });
        (void)future.valid();
    }

    const auto elapsed = std::chrono::steady_clock::now() - start;
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();

    EXPECT_GE(ms, 220);
}

}  