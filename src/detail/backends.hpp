#pragma once

#include "intentio/execution.hpp"
#include "planner.hpp"
#include "submission_state.hpp"

#include <cstddef>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <system_error>
#include <thread>
#include <vector>

namespace intentio::detail {

struct ExecutionStats {
    std::size_t completed{};
    std::size_t peak_in_flight{};
    std::uint64_t elapsed_ns{};
    std::uint64_t total_latency_ns{};
};

void execute_pread(PhysicalRead& read);

struct ExecutorMetricsSnapshot {
    std::uint64_t tasks_submitted{};
    std::uint64_t tasks_completed{};
    std::uint64_t queue_depth{};
    std::uint64_t queue_wait_ns{};
    std::uint64_t execution_ns{};
    std::uint64_t completion_dispatch_ns{};
    std::uint64_t worker_wakeups{};
    std::uint64_t spurious_wakeups{};
    std::uint64_t workers_busy{};
    std::uint64_t workers_idle{};
    std::uint64_t max_queue_depth{};
    std::uint64_t peak_in_flight{};
};

class SyncBackend {
public:
    ExecutionStats execute(std::vector<PhysicalRead>& reads,
                           const intentio::ExecutionPolicy& policy) const;
};

class ThreadedPreadBackend {
public:
    using CompletionCallback =
        std::function<void(const std::shared_ptr<SubmissionState>&)>;

    ThreadedPreadBackend(std::size_t worker_count,
                         std::size_t max_in_flight,
                         std::size_t batch_size);
    ~ThreadedPreadBackend();

    ThreadedPreadBackend(const ThreadedPreadBackend&) = delete;
    ThreadedPreadBackend& operator=(const ThreadedPreadBackend&) = delete;

    void submit(const std::shared_ptr<SubmissionState>& submission,
                CompletionCallback on_complete);
    void set_max_in_flight(std::size_t max_in_flight);
    [[nodiscard]] ExecutorMetricsSnapshot metrics() const noexcept;

private:
    struct Task {
        std::shared_ptr<SubmissionState> submission;
        std::size_t index{};
        std::uint64_t available_at_ns{};
    };

    [[nodiscard]] bool has_work_locked() const;
    [[nodiscard]] Task next_task_locked();
    void finish_tasks(const std::vector<Task>& tasks,
                      const std::vector<std::uint64_t>& latencies_ns);
    void worker_loop();

    std::size_t worker_count_{};
    std::vector<std::thread> workers_;
    std::deque<std::shared_ptr<SubmissionState>> active_;
    mutable std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    bool stopping_{false};
    std::size_t max_in_flight_{};
    std::size_t batch_size_{};
    std::size_t in_flight_{};
    std::size_t priority_burst_{};

    std::uint64_t tasks_submitted_{};
    std::uint64_t tasks_completed_{};
    std::uint64_t queue_wait_ns_{};
    std::uint64_t execution_ns_{};
    std::uint64_t completion_dispatch_ns_{};
    std::uint64_t worker_wakeups_{};
    std::uint64_t spurious_wakeups_{};
    std::uint64_t workers_busy_{};
    std::uint64_t workers_idle_{};
    std::uint64_t max_queue_depth_{};
    std::uint64_t peak_in_flight_{};
};

class IoUringBackend {
public:
    struct Impl;

    explicit IoUringBackend(std::size_t queue_depth);
    IoUringBackend(const IoUringBackend&) = delete;
    IoUringBackend& operator=(const IoUringBackend&) = delete;
    ~IoUringBackend();

    ExecutionStats execute(std::vector<PhysicalRead>& reads,
                           const intentio::ExecutionPolicy& policy);

private:
    Impl* impl_{};
};

} // namespace intentio::detail
