#pragma once

#include <atomic>
#include <cerrno>

#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>

class FutexMutex {
public:
    FutexMutex() = default;

    FutexMutex(const FutexMutex&) = delete;
    FutexMutex& operator=(const FutexMutex&) = delete;

    void lock() {
        int expected = 0;
        if (state_.compare_exchange_strong(expected, 1, std::memory_order_acquire, std::memory_order_relaxed)) {
            return;
        }

        while (state_.exchange(2, std::memory_order_acquire) != 0) {
            FutexWait(2);
        }
    }

    bool try_lock() {
        int expected = 0;
        return state_.compare_exchange_strong(expected, 1, std::memory_order_acquire, std::memory_order_relaxed);
    }

    void unlock() {
        const int previous = state_.fetch_sub(1, std::memory_order_release);
        if (previous != 1) {
            state_.store(0, std::memory_order_release);
            FutexWakeOne();
        }
    }

private:
    void FutexWait(const int expected) {
        while (syscall(SYS_futex, &state_, FUTEX_WAIT_PRIVATE, expected, nullptr, nullptr, 0) == -1 && errno == EINTR) {
        }
    }

    void FutexWakeOne() {
        syscall(SYS_futex, &state_, FUTEX_WAKE_PRIVATE, 1, nullptr, nullptr, 0);
    }

    // 0 -> unlocked, 1 -> locked (no known waiters), 2 -> locked (contended)
    std::atomic<int> state_{0};
};
