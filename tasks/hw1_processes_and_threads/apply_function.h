#pragma once

#include <algorithm>
#include <cstddef>
#include <functional>
#include <thread>
#include <vector>

template <typename T>
void ApplyFunction(std::vector<T>& data, const std::function<void(T&)>& transform, const int threadCount = 1) {
    if (data.empty()) {
        return;
    }

    const int normalizedThreads = std::clamp(threadCount, 1, static_cast<int>(data.size()));
    if (normalizedThreads == 1) {
        for (T& value : data) {
            transform(value);
        }
        return;
    }

    const std::size_t dataSize = data.size();
    const std::size_t threads = static_cast<std::size_t>(normalizedThreads);
    const std::size_t chunkSize = dataSize / threads;
    const std::size_t remainder = dataSize % threads;

    std::vector<std::thread> workers;
    workers.reserve(threads);

    std::size_t begin = 0;
    for (std::size_t i = 0; i < threads; ++i) {
        const std::size_t extra = i < remainder ? 1 : 0;
        const std::size_t end = begin + chunkSize + extra;

        workers.emplace_back([begin, end, &data, &transform]() {
            for (std::size_t index = begin; index < end; ++index) {
                transform(data[index]);
            }
        });

        begin = end;
    }

    for (std::thread& worker : workers) {
        worker.join();
    }
}
