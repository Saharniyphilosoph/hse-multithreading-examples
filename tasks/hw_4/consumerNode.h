#pragma once

#include "ipc_queue_common.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <type_traits>
#include <vector>

class ConsumerNode {
public:
    explicit ConsumerNode(const std::string& queue_name)
        : queue_(queue_name, 0, hw4::ipc::SharedMpscQueue::OpenMode::OpenExistingConsumer) {
    }

    bool TryPop(std::uint32_t expected_type, std::vector<std::byte>& payload) {
        return queue_.TryPopFiltered(expected_type, payload);
    }

    bool TryPopString(std::uint32_t expected_type, std::string& payload) {
        std::vector<std::byte> bytes;
        if (!TryPop(expected_type, bytes)) {
            return false;
        }

        if (bytes.empty()) {
            payload.clear();
        } else {
            payload.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        }
        return true;
    }

    template <typename T>
    bool TryPopPod(std::uint32_t expected_type, T& value) {
        static_assert(std::is_trivially_copyable_v<T>, "must be trivially copyable");

        std::vector<std::byte> bytes;
        if (!TryPop(expected_type, bytes)) {
            return false;
        }

        if (bytes.size() != sizeof(T)) {
            return false;
        }

        std::memcpy(&value, bytes.data(), sizeof(T));
        return true;
    }

private:
    hw4::ipc::SharedMpscQueue queue_;
};

