#pragma once

#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/mman.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace hw4::ipc {

inline constexpr std::uint64_t kQueueMagic = 0x4857345f49504331ULL;  // "hw_4_ipc"
inline constexpr std::uint32_t kProtocolVersion = 1;
inline constexpr std::uint32_t kPaddingMessageType = 0xFFFFFFFFu;
inline constexpr std::uint64_t kRecordAlignment = 8;
inline constexpr std::uint32_t kQueueReady = 1;

inline std::uint64_t AlignUp(std::uint64_t value, std::uint64_t alignment) {
    return (value + alignment - 1) / alignment * alignment;
}

inline std::string NormalizeShmName(std::string_view name) {
    if (name.empty()) {
        throw std::invalid_argument("memory name must not be empty");
    }

    if (name.front() == '/') {
        return std::string(name);
    }

    std::string normalized;
    normalized.reserve(name.size() + 1);
    normalized.push_back('/');
    normalized.append(name);
    return normalized;
}

inline void UnlinkQueue(std::string_view name) {
    const std::string normalized = NormalizeShmName(name);
    (void)shm_unlink(normalized.c_str());
}

struct MessageHeader {
    std::uint32_t type;
    std::uint32_t length;
};

struct alignas(64) QueueMeta {
    std::atomic<std::uint32_t> ready;
    std::uint32_t reserved0;

    std::uint64_t magic;
    std::uint32_t version;
    std::uint32_t reserved1;

    std::uint64_t capacity;

    std::atomic<std::uint64_t> head;
    std::atomic<std::uint64_t> tail_reserved;
    std::atomic<std::uint64_t> tail_committed;
};

class SharedMpscQueue {
public:
    enum class OpenMode {
        CreateOrOpenProducer,
        OpenExistingConsumer,
    };

    SharedMpscQueue(std::string_view name, std::size_t requested_capacity, OpenMode mode) {
        name_ = NormalizeShmName(name);
        Open(requested_capacity, mode);
    }

    SharedMpscQueue(const SharedMpscQueue&) = delete;
    SharedMpscQueue& operator=(const SharedMpscQueue&) = delete;

    SharedMpscQueue(SharedMpscQueue&& other) noexcept {
        MoveFrom(std::move(other));
    }

    SharedMpscQueue& operator=(SharedMpscQueue&& other) noexcept {
        if (this != &other) {
            Close();
            MoveFrom(std::move(other));
        }
        return *this;
    }

    ~SharedMpscQueue() {
        Close();
    }

    bool TryPush(std::uint32_t type, std::span<const std::byte> payload) {
        if (type == kPaddingMessageType) {
            return false;
        }
        if (payload.size() > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
            return false;
        }

        const std::uint64_t payload_size = payload.size();
        const std::uint64_t record_size = AlignUp(sizeof(MessageHeader) + payload_size, kRecordAlignment);

        if (record_size > capacity_) {
            return false;
        }

        while (true) {
            const std::uint64_t head = meta_->head.load(std::memory_order_acquire);
            std::uint64_t tail = meta_->tail_reserved.load(std::memory_order_relaxed);

            const std::uint64_t index = tail % capacity_;
            const std::uint64_t contiguous = capacity_ - index;

            std::uint64_t padding = 0;
            if (contiguous < sizeof(MessageHeader) || contiguous < record_size) {
                padding = contiguous;
            }

            const std::uint64_t required = padding + record_size;
            if (tail - head + required > capacity_) {
                return false;
            }

            const std::uint64_t new_tail = tail + required;
            if (!meta_->tail_reserved.compare_exchange_weak(
                    tail,
                    new_tail,
                    std::memory_order_acq_rel,
                    std::memory_order_relaxed)) {
                std::this_thread::yield();
                continue;
            }

            if (padding >= sizeof(MessageHeader)) {
                WritePaddingRecord(tail, padding);
            }

            const std::uint64_t message_offset = tail + padding;
            WriteDataRecord(message_offset, type, payload);
            PublishCommittedRange(tail, new_tail);
            return true;
        }
    }

    bool TryPopFiltered(std::uint32_t expected_type, std::vector<std::byte>& payload) {
        while (true) {
            const std::uint64_t head = meta_->head.load(std::memory_order_relaxed);
            const std::uint64_t committed = meta_->tail_committed.load(std::memory_order_acquire);

            if (head >= committed) {
                return false;
            }

            const std::uint64_t index = head % capacity_;
            const std::uint64_t contiguous = capacity_ - index;

            if (contiguous < sizeof(MessageHeader)) {
                meta_->head.store(head + contiguous, std::memory_order_release);
                continue;
            }

            const auto* header = reinterpret_cast<const MessageHeader*>(ring_ + index);
            const std::uint64_t payload_size = header->length;
            const std::uint64_t record_size = AlignUp(sizeof(MessageHeader) + payload_size, kRecordAlignment);

            if (record_size > contiguous || head + record_size > committed || payload_size > capacity_) {
                throw std::runtime_error("invalid record size");
            }

            if (header->type == kPaddingMessageType) {
                meta_->head.store(head + record_size, std::memory_order_release);
                continue;
            }

            if (header->type == expected_type) {
                payload.resize(static_cast<std::size_t>(payload_size));
                if (payload_size > 0) {
                    std::memcpy(payload.data(), header + 1, static_cast<std::size_t>(payload_size));
                }
                meta_->head.store(head + record_size, std::memory_order_release);
                return true;
            }

            meta_->head.store(head + record_size, std::memory_order_release);
        }
    }

    [[nodiscard]] std::size_t capacity() const {
        return static_cast<std::size_t>(capacity_);
    }

private:
    static std::runtime_error SystemError(const std::string& message) {
        return std::runtime_error(message + ": " + std::strerror(errno));
    }

    void Open(std::size_t requested_capacity, OpenMode mode) {
        bool created = false;

        if (mode == OpenMode::CreateOrOpenProducer) {
            fd_ = shm_open(name_.c_str(), O_RDWR | O_CREAT | O_EXCL, 0666);
            if (fd_ >= 0) {
                created = true;
            } else if (errno == EEXIST) {
                fd_ = shm_open(name_.c_str(), O_RDWR, 0666);
                if (fd_ < 0) {
                    throw SystemError("failed to open existing memory for producer");
                }
            } else {
                throw SystemError("failed to create memory");
            }
        } else {
            fd_ = shm_open(name_.c_str(), O_RDWR, 0666);
            if (fd_ < 0) {
                throw SystemError("failed to open memory for consumer");
            }
        }

        if (created) {
            if (requested_capacity == 0) {
                throw std::invalid_argument("requested capacity must be greater than zero");
            }

            capacity_ = AlignUp(static_cast<std::uint64_t>(requested_capacity), kRecordAlignment);
            mapping_size_ = sizeof(QueueMeta) + static_cast<std::size_t>(capacity_);

            if (ftruncate(fd_, static_cast<off_t>(mapping_size_)) != 0) {
                throw SystemError("ftruncate failed");
            }

            MapMemory();
            InitializeMeta();
            return;
        }

        struct stat st {};
        if (fstat(fd_, &st) != 0) {
            throw SystemError("fstat failed");
        }

        if (st.st_size < static_cast<off_t>(sizeof(QueueMeta))) {
            throw std::runtime_error("shared memory object is too small");
        }

        mapping_size_ = static_cast<std::size_t>(st.st_size);
        MapMemory();
        WaitUntilReady();
        ValidateMeta();

        if (mode == OpenMode::CreateOrOpenProducer && requested_capacity > 0) {
            const std::uint64_t aligned_requested = AlignUp(static_cast<std::uint64_t>(requested_capacity), kRecordAlignment);
            if (aligned_requested != capacity_) {
                throw std::runtime_error("existing queue has different capacity");
            }
        }
    }

    void MapMemory() {
        mapping_ = mmap(nullptr, mapping_size_, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
        if (mapping_ == MAP_FAILED) {
            mapping_ = nullptr;
            throw SystemError("mmap failed");
        }

        meta_ = reinterpret_cast<QueueMeta*>(mapping_);
        ring_ = reinterpret_cast<std::byte*>(mapping_) + sizeof(QueueMeta);
    }

    void InitializeMeta() {
        std::memset(mapping_, 0, mapping_size_);

        meta_->ready.store(0, std::memory_order_relaxed);
        meta_->magic = kQueueMagic;
        meta_->version = kProtocolVersion;
        meta_->capacity = capacity_;

        meta_->head.store(0, std::memory_order_relaxed);
        meta_->tail_reserved.store(0, std::memory_order_relaxed);
        meta_->tail_committed.store(0, std::memory_order_relaxed);

        std::atomic_thread_fence(std::memory_order_release);
        meta_->ready.store(kQueueReady, std::memory_order_release);
    }

    void WaitUntilReady() const {
        constexpr int kMaxSpins = 10000;
        for (int i = 0; i < kMaxSpins; ++i) {
            if (meta_->ready.load(std::memory_order_acquire) == kQueueReady) {
                return;
            }
            std::this_thread::yield();
        }

        throw std::runtime_error("queue is not initialized yet");
    }

    void ValidateMeta() {
        if (meta_->magic != kQueueMagic) {
            throw std::runtime_error("queue magic mismatch");
        }
        if (meta_->version != kProtocolVersion) {
            throw std::runtime_error("queue protocol version mismatch");
        }
        if (meta_->capacity == 0) {
            throw std::runtime_error("queue capacity must be positive");
        }

        capacity_ = meta_->capacity;
        const std::size_t expected_size = sizeof(QueueMeta) + static_cast<std::size_t>(capacity_);
        if (mapping_size_ < expected_size) {
            throw std::runtime_error("mapped shared memory is smaller than queue capacity");
        }
    }

    void WritePaddingRecord(std::uint64_t absolute_offset, std::uint64_t padding_size) {
        auto* header = reinterpret_cast<MessageHeader*>(PointerAt(absolute_offset));
        header->type = kPaddingMessageType;
        header->length = static_cast<std::uint32_t>(padding_size - sizeof(MessageHeader));
    }

    void WriteDataRecord(std::uint64_t absolute_offset, std::uint32_t type, std::span<const std::byte> payload) {
        auto* header = reinterpret_cast<MessageHeader*>(PointerAt(absolute_offset));
        header->type = type;
        header->length = static_cast<std::uint32_t>(payload.size());

        if (!payload.empty()) {
            std::memcpy(header + 1, payload.data(), payload.size());
        }
    }

    void PublishCommittedRange(std::uint64_t begin, std::uint64_t end) {
        while (true) {
            std::uint64_t expected = begin;
            if (meta_->tail_committed.compare_exchange_weak(
                    expected,
                    end,
                    std::memory_order_release,
                    std::memory_order_acquire)) {
                return;
            }
            std::this_thread::yield();
        }
    }

    std::byte* PointerAt(std::uint64_t absolute_offset) const {
        return ring_ + (absolute_offset % capacity_);
    }

    void Close() {
        if (mapping_ != nullptr) {
            munmap(mapping_, mapping_size_);
            mapping_ = nullptr;
        }

        if (fd_ >= 0) {
            close(fd_);
            fd_ = -1;
        }

        mapping_size_ = 0;
        capacity_ = 0;
        meta_ = nullptr;
        ring_ = nullptr;
    }

    void MoveFrom(SharedMpscQueue&& other) {
        name_ = std::move(other.name_);
        fd_ = other.fd_;
        mapping_ = other.mapping_;
        mapping_size_ = other.mapping_size_;
        meta_ = other.meta_;
        ring_ = other.ring_;
        capacity_ = other.capacity_;

        other.fd_ = -1;
        other.mapping_ = nullptr;
        other.mapping_size_ = 0;
        other.meta_ = nullptr;
        other.ring_ = nullptr;
        other.capacity_ = 0;
    }

private:
    std::string name_;
    int fd_ = -1;
    void* mapping_ = nullptr;
    std::size_t mapping_size_ = 0;

    QueueMeta* meta_ = nullptr;
    std::byte* ring_ = nullptr;
    std::uint64_t capacity_ = 0;
};

}  
