#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <errno.h>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace hw5 {

class FutureCancelled : public std::runtime_error {
public:
    explicit FutureCancelled(const std::string& message)
        : std::runtime_error(message) {
    }
};

template <class T>
class SharedState {
public:
    using Storage = std::conditional_t<std::is_void_v<T>, std::monostate, T>;

    bool SetValue(Storage value = Storage{}) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (ready_) {
            return false;
        }

        value_ = std::move(value);
        ready_ = true;
        cv_.notify_all();
        return true;
    }

    bool SetException(std::exception_ptr exception) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (ready_) {
            return false;
        }

        exception_ = std::move(exception);
        ready_ = true;
        cv_.notify_all();
        return true;
    }

    void Wait() const {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this]() { return ready_; });
    }

    template <class Rep, class Period>
    bool WaitFor(const std::chrono::duration<Rep, Period>& timeout) const {
        std::unique_lock<std::mutex> lock(mutex_);
        return cv_.wait_for(lock, timeout, [this]() { return ready_; });
    }

    Storage Take() {
        Wait();

        std::lock_guard<std::mutex> lock(mutex_);
        if (consumed_) {
            throw std::logic_error("Future result was already retrieved");
        }

        consumed_ = true;
        if (exception_) {
            std::rethrow_exception(exception_);
        }

        return std::move(*value_);
    }

private:
    mutable std::mutex mutex_;
    mutable std::condition_variable cv_;
    bool ready_{false};
    bool consumed_{false};
    std::optional<Storage> value_;
    std::exception_ptr exception_;
};

template <class T>
class Future;

template <class ValueType, class ContinuationType>
struct ContinuationResultType {
    using type = std::invoke_result_t<ContinuationType, ValueType>;
};

template <class ContinuationType>
struct ContinuationResultType<void, ContinuationType> {
    using type = std::invoke_result_t<ContinuationType>;
};

template <class T>
class Promise {
public:
    Promise()
        : state_(std::make_shared<SharedState<T>>()) {
    }

    Promise(const Promise&) = delete;
    Promise& operator=(const Promise&) = delete;
    Promise(Promise&&) noexcept = default;
    Promise& operator=(Promise&&) noexcept = default;

    Future<T> GetFuture() {
        if (future_taken_) {
            throw std::logic_error("Future already requested");
        }

        future_taken_ = true;
        return Future<T>(state_);
    }

    template <class U = T>
    requires(std::is_void_v<U> == false)
    void SetValue(U value) {
        state_->SetValue(std::move(value));
    }

    template <class U = T>
    requires(std::is_void_v<U>)
    void SetValue() {
        state_->SetValue(std::monostate{});
    }

    void SetException(std::exception_ptr exception) {
        state_->SetException(std::move(exception));
    }

private:
    std::shared_ptr<SharedState<T>> state_;
    bool future_taken_{false};
};

template <class T>
class Future {
public:
    Future() = default;

    Future(const Future&) = delete;
    Future& operator=(const Future&) = delete;
    Future(Future&&) noexcept = default;
    Future& operator=(Future&&) noexcept = default;

    bool valid() const noexcept {
        return state_ != nullptr;
    }

    void wait() const {
        EnsureValid();
        state_->Wait();
    }

    template <class Rep, class Period>
    bool wait_for(const std::chrono::duration<Rep, Period>& timeout) const {
        EnsureValid();
        return state_->WaitFor(timeout);
    }

    bool Cancel() noexcept {
        if (!cancel_callback_) {
            return false;
        }

        try {
            return cancel_callback_();
        } catch (...) {
            return false;
        }
    }

    T get() {
        EnsureValid();

        if constexpr (std::is_void_v<T>) {
            static_cast<void>(state_->Take());
            state_.reset();
            return;
        } else {
            auto value = state_->Take();
            state_.reset();
            return value;
        }
    }

    template <class Fn>
    auto Then(Fn&& continuation) {
        using ContinuationType = std::decay_t<Fn>;
        using NextType = typename ContinuationResultType<T, ContinuationType>::type;

        Promise<NextType> promise;
        auto next = promise.GetFuture();

        auto previous = std::move(*this);
        std::thread([previous = std::move(previous),
                     promise = std::move(promise),
                     continuation = ContinuationType(std::forward<Fn>(continuation))]() mutable {
            try {
                if constexpr (std::is_void_v<T>) {
                    previous.get();
                    if constexpr (std::is_void_v<NextType>) {
                        continuation();
                        promise.SetValue();
                    } else {
                        promise.SetValue(continuation());
                    }
                } else {
                    auto value = previous.get();
                    if constexpr (std::is_void_v<NextType>) {
                        continuation(std::move(value));
                        promise.SetValue();
                    } else {
                        promise.SetValue(continuation(std::move(value)));
                    }
                }
            } catch (...) {
                promise.SetException(std::current_exception());
            }
        }).detach();

        return next;
    }

private:
    friend class Promise<T>;
    friend class ProcessPool;

    explicit Future(std::shared_ptr<SharedState<T>> state)
        : state_(std::move(state)) {
    }

    void SetCancelCallback(std::function<bool()> cancel_callback) {
        cancel_callback_ = std::move(cancel_callback);
    }

    void EnsureValid() const {
        if (!state_) {
            throw std::logic_error("Future has no state");
        }
    }

private:
    std::shared_ptr<SharedState<T>> state_;
    std::function<bool()> cancel_callback_;
};

class ProcessPool {
public:
    explicit ProcessPool(std::size_t max_processes)
        : worker_count_(std::max<std::size_t>(1, max_processes)) {
        workers_.resize(worker_count_);
        for (std::size_t i = 0; i < worker_count_; ++i) {
            SpawnWorker(i);
        }

        dispatcher_thread_ = std::jthread([this]() { DispatchLoop(); });
    }

    ProcessPool(const ProcessPool&) = delete;
    ProcessPool& operator=(const ProcessPool&) = delete;

    ~ProcessPool() noexcept {
        Shutdown();
    }

    void Shutdown() noexcept {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (shutdown_) {
                return;
            }
            accepting_ = false;
        }
        cv_.notify_all();

        if (dispatcher_thread_.joinable()) {
            dispatcher_thread_.join();
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            shutdown_ = true;
        }
        cv_.notify_all();

        for (auto& worker : workers_) {
            if (worker.reader.joinable()) {
                worker.reader.join();
            }
        }

        for (auto& worker : workers_) {
            if (worker.to_worker_fd >= 0) {
                close(worker.to_worker_fd);
                worker.to_worker_fd = -1;
            }
            if (worker.from_worker_fd >= 0) {
                close(worker.from_worker_fd);
                worker.from_worker_fd = -1;
            }
            if (worker.pid > 0) {
                int status = 0;
                while (waitpid(worker.pid, &status, 0) == -1 && errno == EINTR) {
                }
                worker.pid = -1;
            }
        }

        std::unordered_map<std::uint64_t, std::shared_ptr<TaskControl>> remaining;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            remaining.swap(tasks_);
            pending_.clear();
        }

        for (auto& [id, task] : remaining) {
            (void)id;
            FinishCancelled(task, "Task cancelled because ProcessPool is shutting down");
        }
    }

    template <class Fn, class... Args>
    auto Submit(Fn&& fn, Args&&... args)
        -> Future<std::invoke_result_t<std::decay_t<Fn>, std::decay_t<Args>...>> {
        using Callable = std::decay_t<Fn>;
        using ArgsTuple = std::tuple<std::decay_t<Args>...>;
        using Result = std::invoke_result_t<Callable, std::decay_t<Args>...>;

        static_assert(std::is_reference_v<Result> == false, "Reference result is not supported");
        static_assert(std::is_void_v<Result> || std::is_trivially_copyable_v<Result>,
                      "Only void or trivially copyable results are supported");
        static_assert(std::is_trivially_copyable_v<Callable>,
                      "Callable must be trivially copyable");
        static_assert(std::is_trivially_copyable_v<ArgsTuple>,
                      "Arguments must be trivially copyable");

        Promise<Result> promise;
        auto future = promise.GetFuture();

        auto promise_holder = std::make_shared<Promise<Result>>(std::move(promise));

        auto task = std::make_shared<TaskControl>();
        task->id = next_task_id_.fetch_add(1, std::memory_order_relaxed);
        task->invoke_ptr = reinterpret_cast<std::uint64_t>(&InvokeTyped<Callable, ArgsTuple, Result>);
        task->function_bytes = ToBytes(Callable(std::forward<Fn>(fn)));
        task->args_bytes = ToBytes(ArgsTuple(std::forward<Args>(args)...));
        task->result_size = static_cast<std::uint32_t>(sizeof(typename SharedState<Result>::Storage));

        task->on_success = [promise_holder](const std::vector<std::uint8_t>& payload) mutable {
            if constexpr (std::is_void_v<Result>) {
                promise_holder->SetValue();
            } else {
                if (payload.size() != sizeof(Result)) {
                    promise_holder->SetException(
                        std::make_exception_ptr(std::runtime_error("Bad payload size for task result")));
                    return;
                }

                Result value{};
                std::memcpy(&value, payload.data(), sizeof(Result));
                promise_holder->SetValue(value);
            }
        };

        task->on_error = [promise_holder](const std::string& message) mutable {
            promise_holder->SetException(std::make_exception_ptr(std::runtime_error(message)));
        };

        task->on_cancel = [promise_holder](const std::string& message) mutable {
            promise_holder->SetException(std::make_exception_ptr(FutureCancelled(message)));
        };

        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (accepting_ == false || broken_ || shutdown_) {
                throw std::runtime_error("ProcessPool is not accepting new tasks");
            }

            tasks_.emplace(task->id, task);
            pending_.push_back(task);
        }

        std::weak_ptr<TaskControl> weak_task = task;
        future.SetCancelCallback([this, weak_task]() noexcept {
            const auto task_locked = weak_task.lock();
            if (!task_locked) {
                return false;
            }

            if (task_locked->finished.load(std::memory_order_acquire)) {
                return false;
            }

            task_locked->cancel_requested.store(true, std::memory_order_release);

            bool cancel_before_start = false;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (task_locked->finished.load(std::memory_order_acquire)) {
                    return false;
                }

                if (task_locked->started.load(std::memory_order_acquire) == false) {
                    auto it = std::find_if(
                        pending_.begin(),
                        pending_.end(),
                        [id = task_locked->id](const std::shared_ptr<TaskControl>& item) {
                            return item && item->id == id;
                        });
                    if (it != pending_.end()) {
                        pending_.erase(it);
                    }
                    cancel_before_start = true;
                }
            }

            if (cancel_before_start) {
                FinishCancelled(task_locked, "Task cancelled before start");
            }

            cv_.notify_all();
            return true;
        });

        cv_.notify_all();
        return future;
    }

private:
    struct TaskHeader {
        std::uint64_t task_id{0};
        std::uint64_t invoke_ptr{0};
        std::uint32_t fn_size{0};
        std::uint32_t args_size{0};
        std::uint32_t result_size{0};
    };

    struct ResultHeader {
        std::uint64_t task_id{0};
        std::uint8_t status{0};
        std::uint32_t payload_size{0};
    };

    struct TaskControl {
        std::uint64_t id{0};
        std::uint64_t invoke_ptr{0};
        std::uint32_t result_size{0};

        std::vector<std::uint8_t> function_bytes;
        std::vector<std::uint8_t> args_bytes;

        std::function<void(const std::vector<std::uint8_t>&)> on_success;
        std::function<void(const std::string&)> on_error;
        std::function<void(const std::string&)> on_cancel;

        std::atomic<bool> cancel_requested{false};
        std::atomic<bool> started{false};
        std::atomic<bool> finished{false};
        int worker_index{-1};
    };

    struct Worker {
        pid_t pid{-1};
        int to_worker_fd{-1};
        int from_worker_fd{-1};

        bool busy{false};
        std::uint64_t current_task_id{0};
        std::jthread reader;
    };

    using InvokeFn = bool (*)(const void*, const void*, void*, char*, std::size_t) noexcept;

    template <class T>
    static std::vector<std::uint8_t> ToBytes(const T& value) {
        std::vector<std::uint8_t> bytes(sizeof(T));
        if constexpr (sizeof(T) > 0) {
            std::memcpy(bytes.data(), &value, sizeof(T));
        }
        return bytes;
    }

    template <class Callable, class ArgsTuple, class Result>
    static bool InvokeTyped(const void* callable_data,
                            const void* args_data,
                            void* result_data,
                            char* error_data,
                            const std::size_t error_capacity) noexcept {
        try {
            Callable callable{};
            ArgsTuple args{};

            std::memcpy(&callable, callable_data, sizeof(Callable));
            std::memcpy(&args, args_data, sizeof(ArgsTuple));

            if constexpr (std::is_void_v<Result>) {
                std::apply(callable, args);
            } else {
                Result result = std::apply(callable, args);
                std::memcpy(result_data, &result, sizeof(Result));
            }
            return true;
        } catch (const std::exception& e) {
            CopyErrorMessage(e.what(), error_data, error_capacity);
            return false;
        } catch (...) {
            CopyErrorMessage("Unknown exception", error_data, error_capacity);
            return false;
        }
    }

    static void CopyErrorMessage(const std::string& message, char* data, const std::size_t capacity) noexcept {
        if (capacity == 0) {
            return;
        }

        const std::size_t size = std::min(capacity - 1, message.size());
        if (size > 0) {
            std::memcpy(data, message.data(), size);
        }
        data[size] = '\0';
    }

    void SpawnWorker(const std::size_t index) {
        int to_worker[2] = {-1, -1};
        int from_worker[2] = {-1, -1};

        if (pipe(to_worker) != 0 || pipe(from_worker) != 0) {
            throw std::runtime_error("Failed to create pipes for worker process");
        }

        const pid_t child = fork();
        if (child == -1) {
            close(to_worker[0]);
            close(to_worker[1]);
            close(from_worker[0]);
            close(from_worker[1]);
            throw std::runtime_error("Failed to fork worker process");
        }

        if (child == 0) {
            close(to_worker[1]);
            close(from_worker[0]);
            WorkerLoop(to_worker[0], from_worker[1]);
            _exit(0);
        }

        close(to_worker[0]);
        close(from_worker[1]);

        workers_[index].pid = child;
        workers_[index].to_worker_fd = to_worker[1];
        workers_[index].from_worker_fd = from_worker[0];
        workers_[index].busy = false;
        workers_[index].current_task_id = 0;

        workers_[index].reader = std::jthread([this, index]() { ReaderLoop(index); });
    }

    void DispatchLoop() noexcept {
        while (true) {
            std::shared_ptr<TaskControl> task;
            std::size_t worker_index = 0;

            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait(lock, [this]() {
                    return broken_ ||
                           (pending_.empty() == false && FindIdleWorkerLocked().has_value()) ||
                           (accepting_ == false && pending_.empty() && AllWorkersIdleLocked());
                });

                if (broken_) {
                    break;
                }

                if (accepting_ == false && pending_.empty() && AllWorkersIdleLocked()) {
                    break;
                }

                task = PopPendingLocked();
                if (!task) {
                    continue;
                }

                if (task->cancel_requested.load(std::memory_order_acquire)) {
                    lock.unlock();
                    FinishCancelled(task, "Task cancelled before start");
                    continue;
                }

                auto maybe_worker = FindIdleWorkerLocked();
                if (!maybe_worker.has_value()) {
                    pending_.push_front(task);
                    continue;
                }

                worker_index = *maybe_worker;
                if (!SendTaskLocked(worker_index, task)) {
                    broken_ = true;
                    accepting_ = false;
                    lock.unlock();
                    FinishError(task, "Failed to send task to worker");
                    continue;
                }
            }
        }

        for (auto& worker : workers_) {
            if (worker.to_worker_fd < 0) {
                continue;
            }

            TaskHeader stop{};
            (void)WriteAll(worker.to_worker_fd, &stop, sizeof(stop));
            close(worker.to_worker_fd);
            worker.to_worker_fd = -1;
        }
    }

    bool SendTaskLocked(const std::size_t worker_index, const std::shared_ptr<TaskControl>& task) {
        TaskHeader header{};
        header.task_id = task->id;
        header.invoke_ptr = task->invoke_ptr;
        header.fn_size = static_cast<std::uint32_t>(task->function_bytes.size());
        header.args_size = static_cast<std::uint32_t>(task->args_bytes.size());
        header.result_size = task->result_size;

        Worker& worker = workers_[worker_index];
        if (worker.to_worker_fd < 0 || worker.pid <= 0) {
            return false;
        }

        if (!WriteAll(worker.to_worker_fd, &header, sizeof(header))) {
            return false;
        }

        if (header.fn_size > 0 &&
            !WriteAll(worker.to_worker_fd, task->function_bytes.data(), header.fn_size)) {
            return false;
        }

        if (header.args_size > 0 &&
            !WriteAll(worker.to_worker_fd, task->args_bytes.data(), header.args_size)) {
            return false;
        }

        task->started.store(true, std::memory_order_release);
        task->worker_index = static_cast<int>(worker_index);

        worker.busy = true;
        worker.current_task_id = task->id;

        return true;
    }

    void ReaderLoop(const std::size_t worker_index) noexcept {
        while (true) {
            ResultHeader header{};
            if (!ReadAll(workers_[worker_index].from_worker_fd, &header, sizeof(header))) {
                HandleWorkerDisconnect(worker_index);
                return;
            }

            std::vector<std::uint8_t> payload(header.payload_size);
            if (header.payload_size > 0 &&
                !ReadAll(workers_[worker_index].from_worker_fd, payload.data(), header.payload_size)) {
                HandleWorkerDisconnect(worker_index);
                return;
            }

            std::shared_ptr<TaskControl> task;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                auto it = tasks_.find(header.task_id);
                if (it != tasks_.end()) {
                    task = it->second;
                }

                workers_[worker_index].busy = false;
                workers_[worker_index].current_task_id = 0;
            }

            if (task) {
                if (task->cancel_requested.load(std::memory_order_acquire)) {
                    FinishCancelled(task, "Task cancelled");
                } else if (header.status == 0) {
                    FinishSuccess(task, payload);
                } else {
                    const std::string error(payload.begin(), payload.end());
                    FinishError(task, error.empty() ? "Task execution error" : error);
                }
            }

            cv_.notify_all();
        }
    }

    void HandleWorkerDisconnect(const std::size_t worker_index) noexcept {
        std::shared_ptr<TaskControl> running_task;

        {
            std::lock_guard<std::mutex> lock(mutex_);
            Worker& worker = workers_[worker_index];

            if (worker.current_task_id != 0) {
                auto it = tasks_.find(worker.current_task_id);
                if (it != tasks_.end()) {
                    running_task = it->second;
                }
            }

            worker.busy = false;
            worker.current_task_id = 0;
            if (worker.from_worker_fd >= 0) {
                close(worker.from_worker_fd);
                worker.from_worker_fd = -1;
            }
            if (worker.to_worker_fd >= 0) {
                close(worker.to_worker_fd);
                worker.to_worker_fd = -1;
            }
            if (worker.pid > 0) {
                int status = 0;
                while (waitpid(worker.pid, &status, 0) == -1 && errno == EINTR) {
                }
                worker.pid = -1;
            }

            if (shutdown_ == false) {
                broken_ = true;
                accepting_ = false;
            }
        }

        if (running_task) {
            if (running_task->cancel_requested.load(std::memory_order_acquire)) {
                FinishCancelled(running_task, "Task cancelled");
            } else {
                FinishError(running_task, "Worker process disconnected");
            }
        }

        cv_.notify_all();
    }

    void FinishSuccess(const std::shared_ptr<TaskControl>& task, const std::vector<std::uint8_t>& payload) {
        bool expected = false;
        if (!task->finished.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            return;
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            tasks_.erase(task->id);
        }

        task->on_success(payload);
    }

    void FinishError(const std::shared_ptr<TaskControl>& task, const std::string& message) {
        bool expected = false;
        if (!task->finished.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            return;
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            tasks_.erase(task->id);
        }

        task->on_error(message);
    }

    void FinishCancelled(const std::shared_ptr<TaskControl>& task, const std::string& message) {
        bool expected = false;
        if (!task->finished.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            return;
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            tasks_.erase(task->id);
        }

        task->on_cancel(message);
    }

    std::optional<std::size_t> FindIdleWorkerLocked() const {
        for (std::size_t i = 0; i < workers_.size(); ++i) {
            if (!workers_[i].busy && workers_[i].pid > 0) {
                return i;
            }
        }
        return std::nullopt;
    }

    bool AllWorkersIdleLocked() const {
        for (const auto& worker : workers_) {
            if (worker.busy) {
                return false;
            }
        }
        return true;
    }

    std::shared_ptr<TaskControl> PopPendingLocked() {
        while (!pending_.empty()) {
            auto task = pending_.front();
            pending_.pop_front();
            if (task) {
                return task;
            }
        }
        return nullptr;
    }

    static void WorkerLoop(const int read_fd, const int write_fd) noexcept {
        while (true) {
            TaskHeader header{};
            if (!ReadAll(read_fd, &header, sizeof(header))) {
                break;
            }

            if (header.task_id == 0 && header.invoke_ptr == 0) {
                break;
            }

            std::vector<std::uint8_t> fn_data(header.fn_size);
            std::vector<std::uint8_t> args_data(header.args_size);

            if (header.fn_size > 0 && !ReadAll(read_fd, fn_data.data(), header.fn_size)) {
                break;
            }
            if (header.args_size > 0 && !ReadAll(read_fd, args_data.data(), header.args_size)) {
                break;
            }

            std::vector<std::uint8_t> result_data(header.result_size);
            char error_buffer[4096] = {0};

            const auto invoke = reinterpret_cast<InvokeFn>(static_cast<std::uintptr_t>(header.invoke_ptr));
            const bool ok = invoke(header.fn_size > 0 ? fn_data.data() : nullptr,
                                   header.args_size > 0 ? args_data.data() : nullptr,
                                   header.result_size > 0 ? result_data.data() : nullptr,
                                   error_buffer,
                                   sizeof(error_buffer));

            ResultHeader reply{};
            reply.task_id = header.task_id;

            if (ok) {
                reply.status = 0;
                reply.payload_size = header.result_size;

                if (!WriteAll(write_fd, &reply, sizeof(reply))) {
                    break;
                }
                if (reply.payload_size > 0 &&
                    !WriteAll(write_fd, result_data.data(), reply.payload_size)) {
                    break;
                }
            } else {
                const std::size_t error_size = std::strlen(error_buffer);
                reply.status = 1;
                reply.payload_size = static_cast<std::uint32_t>(error_size);

                if (!WriteAll(write_fd, &reply, sizeof(reply))) {
                    break;
                }
                if (reply.payload_size > 0 &&
                    !WriteAll(write_fd, error_buffer, reply.payload_size)) {
                    break;
                }
            }
        }

        close(read_fd);
        close(write_fd);
    }

    static bool ReadAll(const int fd, void* data, const std::size_t size) noexcept {
        if (size == 0) {
            return true;
        }

        auto* ptr = static_cast<std::uint8_t*>(data);
        std::size_t left = size;

        while (left > 0) {
            const ssize_t rc = read(fd, ptr, left);
            if (rc == 0) {
                return false;
            }
            if (rc < 0) {
                if (errno == EINTR) {
                    continue;
                }
                return false;
            }

            ptr += rc;
            left -= static_cast<std::size_t>(rc);
        }

        return true;
    }

    static bool WriteAll(const int fd, const void* data, const std::size_t size) noexcept {
        if (size == 0) {
            return true;
        }

        const auto* ptr = static_cast<const std::uint8_t*>(data);
        std::size_t left = size;

        while (left > 0) {
            const ssize_t rc = write(fd, ptr, left);
            if (rc < 0) {
                if (errno == EINTR) {
                    continue;
                }
                return false;
            }

            ptr += rc;
            left -= static_cast<std::size_t>(rc);
        }

        return true;
    }

private:
    std::size_t worker_count_{1};
    std::atomic<std::uint64_t> next_task_id_{1};

    bool accepting_{true};
    bool shutdown_{false};
    bool broken_{false};

    std::mutex mutex_;
    std::condition_variable cv_;

    std::deque<std::shared_ptr<TaskControl>> pending_;
    std::unordered_map<std::uint64_t, std::shared_ptr<TaskControl>> tasks_;
    std::vector<Worker> workers_;

    std::jthread dispatcher_thread_;
};

} 
