#pragma once

#include "ipc_queue_common.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

class ProducerNode {
public:
    ProducerNode(const std::string& queue_name, std::size_t queue_capacity_bytes)
        : queue_(queue_name, queue_capacity_bytes, hw4::ipc::SharedMpscQueue::OpenMode::CreateOrOpenProducer) {
    }

    bool TryPush(std::uint32_t type, std::span<const std::byte> payload) {
        return queue_.TryPush(type, payload);
    }

    bool TryPush(std::uint32_t type, const void* data, std::size_t size) {
        if (data == nullptr && size != 0) {
            return false;
        }

        const auto* bytes = static_cast<const std::byte*>(data);
        return queue_.TryPush(type, std::span<const std::byte>(bytes, size));
    }

    bool TryPushString(std::uint32_t type, std::string_view message) {
        return TryPush(type, message.data(), message.size());
    }

    [[nodiscard]] std::size_t Capacity() const {
        return queue_.capacity();
    }

private:
    hw4::ipc::SharedMpscQueue queue_;
};

