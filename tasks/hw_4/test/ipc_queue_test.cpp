#include "consumerNode.h"
#include "ipc_queue_common.h"
#include "producerNode.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <stdexcept>
#include <string>
#include <utility>
#include <string_view>
#include <sys/mman.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

std::string UniqueQueueName(std::string_view suffix) {
    static std::atomic<std::uint64_t> counter{0};

    return "/hw4_ipc_test_" + std::to_string(getpid()) + "_" +
           std::to_string(counter.fetch_add(1, std::memory_order_relaxed)) + "_" + std::string(suffix);
}

class QueueGuard {
public:
    explicit QueueGuard(std::string queue_name) : queue_name_(std::move(queue_name)) {
        hw4::ipc::UnlinkQueue(queue_name_);
    }

    ~QueueGuard() {
        hw4::ipc::UnlinkQueue(queue_name_);
    }

    const std::string& name() const {
        return queue_name_;
    }

private:
    std::string queue_name_;
};

bool PopStringWithRetries(ConsumerNode& consumer,
                          std::uint32_t expected_type,
                          std::string& payload,
                          int retries = 1000) {
    for (int attempt = 0; attempt < retries; ++attempt) {
        if (consumer.TryPopString(expected_type, payload)) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    return false;
}

void OverwriteQueueVersion(const std::string& queue_name, std::uint32_t version) {
    const std::string normalized_name = hw4::ipc::NormalizeShmName(queue_name);

    const int fd = shm_open(normalized_name.c_str(), O_RDWR, 0666);
    if (fd < 0) {
        throw std::runtime_error("failed to open shared memory for version overwrite");
    }

    struct stat st {};
    if (fstat(fd, &st) != 0) {
        close(fd);
        throw std::runtime_error("failed to stat shared memory for version overwrite");
    }

    void* mapping = mmap(nullptr, st.st_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mapping == MAP_FAILED) {
        close(fd);
        throw std::runtime_error("failed to mmap shared memory for version overwrite");
    }

    auto* meta = reinterpret_cast<hw4::ipc::QueueMeta*>(mapping);
    meta->version = version;

    if (msync(mapping, sizeof(hw4::ipc::QueueMeta), MS_SYNC) != 0) {
        munmap(mapping, st.st_size);
        close(fd);
        throw std::runtime_error("failed to msync shared memory for version overwrite");
    }

    munmap(mapping, st.st_size);
    close(fd);
}

struct WorkItem {
    std::uint32_t producer_id;
    std::uint32_t sequence;
};

}  // namespace

TEST(IpcQueue, BasicPushPopSameType) {
    QueueGuard guard(UniqueQueueName("base"));

    ProducerNode producer(guard.name(), 4096);
    ConsumerNode consumer(guard.name());

    const std::string message = "hello_from_producer";

    ASSERT_TRUE(producer.TryPushString(1, message));

    std::string received;
    ASSERT_TRUE(PopStringWithRetries(consumer, 1, received));
    EXPECT_EQ(received, message);
}

TEST(IpcQueue, ConsumerSkipsOtherTypes) {
    QueueGuard guard(UniqueQueueName("filter"));

    ProducerNode producer(guard.name(), 4096);
    ConsumerNode consumer(guard.name());

    ASSERT_TRUE(producer.TryPushString(2, "drop_me"));
    ASSERT_TRUE(producer.TryPushString(1, "keep_me"));
    ASSERT_TRUE(producer.TryPushString(3, "drop_me_too"));

    std::string received;
    ASSERT_TRUE(PopStringWithRetries(consumer, 1, received));
    EXPECT_EQ(received, "keep_me");

    std::string nothing;
    EXPECT_FALSE(PopStringWithRetries(consumer, 1, nothing, 20));
}

TEST(IpcQueue, QueueFullReturnsFalse) {
    QueueGuard guard(UniqueQueueName("full"));

    ProducerNode producer(guard.name(), 256);
    const std::string payload(48, 'x');

    std::size_t pushed = 0;
    while (pushed < 1024 && producer.TryPushString(7, payload)) {
        ++pushed;
    }

    EXPECT_GT(pushed, 0u);
    EXPECT_LT(pushed, 1024u);
    EXPECT_FALSE(producer.TryPushString(7, payload));
}

TEST(IpcQueue, WrapAroundWorks) {
    QueueGuard guard(UniqueQueueName("wrap"));

    ProducerNode producer(guard.name(), 256);
    ConsumerNode consumer(guard.name());

    const std::string small_payload(32, 'a');
    for (int i = 0; i < 5; ++i) {
        ASSERT_TRUE(producer.TryPushString(9, small_payload));

        std::string received;
        ASSERT_TRUE(PopStringWithRetries(consumer, 9, received));
        ASSERT_EQ(received, small_payload);
    }

    const std::string wrap_payload(64, 'w');
    ASSERT_TRUE(producer.TryPushString(9, wrap_payload));

    std::string received;
    ASSERT_TRUE(PopStringWithRetries(consumer, 9, received));
    EXPECT_EQ(received, wrap_payload);
}

TEST(IpcQueue, VersionMismatchThrows) {
    QueueGuard guard(UniqueQueueName("version"));

    ProducerNode producer(guard.name(), 4096);
    ASSERT_NO_THROW(OverwriteQueueVersion(guard.name(), hw4::ipc::kProtocolVersion + 1));

    EXPECT_THROW({
        ConsumerNode consumer(guard.name());
    },
                 std::runtime_error);
}

TEST(IpcQueue, MultiProducerSingleConsumerStress) {
    QueueGuard guard(UniqueQueueName("stress"));

    ProducerNode producer(guard.name(), 1ULL << 20);
    ConsumerNode consumer(guard.name());

    constexpr std::uint32_t kType = 42;
    constexpr std::uint32_t kProducers = 4;
    constexpr std::uint32_t kMessagesPerProducer = 2000;
    constexpr std::size_t kTotalMessages = static_cast<std::size_t>(kProducers) * kMessagesPerProducer;

    std::vector<std::jthread> producers;
    producers.reserve(kProducers);

    for (std::uint32_t producer_id = 0; producer_id < kProducers; ++producer_id) {
        producers.emplace_back([&, producer_id] {
            for (std::uint32_t sequence = 0; sequence < kMessagesPerProducer; ++sequence) {
                const WorkItem item{producer_id, sequence};

                while (!producer.TryPush(kType, &item, sizeof(item))) {
                    std::this_thread::yield();
                }
            }
        });
    }

    std::vector<std::uint32_t> next_expected(kProducers, 0);

    std::size_t received_total = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);

    while (received_total < kTotalMessages) {
        WorkItem item{};
        if (consumer.TryPopPod(kType, item)) {
            ASSERT_LT(item.producer_id, kProducers);
            EXPECT_EQ(item.sequence, next_expected[item.producer_id]);
            ++next_expected[item.producer_id];
            ++received_total;
            continue;
        }

        if (std::chrono::steady_clock::now() > deadline) {
            FAIL() << "timeout while waiting for consumer to receive all messages";
            break;
        }

        std::this_thread::yield();
    }

    for (std::uint32_t producer_id = 0; producer_id < kProducers; ++producer_id) {
        EXPECT_EQ(next_expected[producer_id], kMessagesPerProducer);
    }
}

