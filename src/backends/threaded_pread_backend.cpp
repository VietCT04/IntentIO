#include "detail/backends.hpp"

#include <algorithm>
#include <chrono>
#include <stdexcept>

namespace intentio::detail {

namespace {

std::uint64_t now_ns() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

void copy_completed_result(SubmissionState& submission, std::size_t index) {
    const PhysicalRead& read = submission.reads[index];
    for (const PhysicalSlice& slice : read.request.slices) {
        ReadResult& result = submission.results[slice.logical_index];
        if (read.error) {
            result.error = read.error;
            continue;
        }
        if (slice.physical_offset >= read.bytes_read) {
            continue;
        }
        const std::size_t available =
            std::min(slice.length, read.bytes_read - slice.physical_offset);
        result.data.resize(available);
        std::copy_n(read.data.data() + slice.physical_offset,
                    available,
                    result.data.data());
    }
}

} // namespace

ThreadedPreadBackend::ThreadedPreadBackend(std::size_t worker_count,
                                           std::size_t max_in_flight,
                                           std::size_t batch_size)
    : worker_count_(worker_count),
      max_in_flight_(max_in_flight),
      batch_size_(batch_size) {
    if (worker_count_ == 0) {
        throw std::invalid_argument("threaded pread worker count must be greater than zero");
    }
    if (max_in_flight_ == 0) {
        throw std::invalid_argument("threaded pread max in-flight must be greater than zero");
    }
    if (batch_size_ == 0) {
        throw std::invalid_argument("threaded pread batch size must be greater than zero");
    }
    max_in_flight_ = std::min(max_in_flight_, worker_count_);
    batch_size_ = std::min(batch_size_, max_in_flight_);

    try {
        workers_.reserve(worker_count_);
        for (std::size_t index = 0; index < worker_count_; ++index) {
            workers_.emplace_back(&ThreadedPreadBackend::worker_loop, this);
        }
    } catch (...) {
        {
            std::lock_guard lock(queue_mutex_);
            stopping_ = true;
        }
        queue_cv_.notify_all();
        for (std::thread& worker : workers_) {
            if (worker.joinable()) {
                worker.join();
            }
        }
        throw;
    }
}

ThreadedPreadBackend::~ThreadedPreadBackend() {
    {
        std::lock_guard lock(queue_mutex_);
        stopping_ = true;
    }
    queue_cv_.notify_all();
    for (std::thread& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
}

bool ThreadedPreadBackend::has_work_locked() const {
    return std::any_of(active_.begin(), active_.end(), [](const auto& submission) {
        return submission->next_index < submission->reads.size();
    });
}

ThreadedPreadBackend::Task ThreadedPreadBackend::next_task_locked() {
    if (active_.empty() || in_flight_ >= max_in_flight_) {
        throw std::logic_error("threaded pread scheduler has no available task");
    }

    bool has_normal = false;
    for (const auto& submission : active_) {
        if (submission->next_index < submission->reads.size() &&
            submission->hints.latency_class != LatencyClass::LatencySensitive) {
            has_normal = true;
            break;
        }
    }

    std::size_t selected_index = active_.size();
    if (priority_burst_ < 4U) {
        for (std::size_t index = 0; index < active_.size(); ++index) {
            const auto& submission = active_[index];
            if (submission->next_index < submission->reads.size() &&
                submission->hints.latency_class ==
                    LatencyClass::LatencySensitive) {
                selected_index = index;
                break;
            }
        }
    }
    if (selected_index == active_.size()) {
        for (std::size_t index = 0; index < active_.size(); ++index) {
            const auto& submission = active_[index];
            if (submission->next_index < submission->reads.size() &&
                (!has_normal || submission->hints.latency_class !=
                                    LatencyClass::LatencySensitive)) {
                selected_index = index;
                break;
            }
        }
    }
    if (selected_index == active_.size()) {
        throw std::logic_error("threaded pread scheduler failed to select a task");
    }

    auto submission = std::move(active_[selected_index]);
    active_.erase(active_.begin() + static_cast<std::ptrdiff_t>(selected_index));
    const std::size_t task_index = submission->next_index++;
    if (submission->next_index < submission->reads.size()) {
        active_.push_back(submission);
    }

    const bool selected_latency =
        submission->hints.latency_class == LatencyClass::LatencySensitive;
    if (selected_latency && has_normal) {
        ++priority_burst_;
    } else if (!selected_latency) {
        priority_burst_ = 0;
    }

    ++in_flight_;
    peak_in_flight_ = std::max(peak_in_flight_,
                               static_cast<std::uint64_t>(in_flight_));
    ++submission->in_flight;
    submission->peak_in_flight =
        std::max(submission->peak_in_flight, submission->in_flight);
    ++tasks_submitted_;

    return Task{submission, task_index, submission->available_at_ns};
}

void ThreadedPreadBackend::submit(
    const std::shared_ptr<SubmissionState>& submission,
    CompletionCallback on_complete) {
    if (!submission) {
        throw std::invalid_argument("threaded pread submission must not be null");
    }

    bool complete_immediately = false;
    {
        std::lock_guard lock(queue_mutex_);
        if (stopping_) {
            throw std::runtime_error("threaded pread backend is shutting down");
        }
        submission->on_complete = std::move(on_complete);
        submission->available_at_ns = now_ns();
        if (submission->reads.empty()) {
            submission->elapsed_ns = 0;
            complete_immediately = true;
        } else {
            active_.push_back(submission);
            std::uint64_t pending = 0;
            for (const auto& active : active_) {
                pending += static_cast<std::uint64_t>(
                    active->reads.size() - active->next_index);
            }
            max_queue_depth_ = std::max(max_queue_depth_, pending);
        }
    }

    if (complete_immediately) {
        if (submission->on_complete) {
            submission->on_complete(submission);
        }
        submission->mark_complete();
        return;
    }
    queue_cv_.notify_all();
}

void ThreadedPreadBackend::set_max_in_flight(std::size_t max_in_flight) {
    if (max_in_flight == 0) {
        throw std::invalid_argument("threaded pread max in-flight must be greater than zero");
    }
    {
        std::lock_guard lock(queue_mutex_);
        max_in_flight_ = std::min(max_in_flight, worker_count_);
    }
    queue_cv_.notify_all();
}

void ThreadedPreadBackend::finish_tasks(
    const std::vector<Task>& tasks,
    const std::vector<std::uint64_t>& latencies_ns) {
    if (tasks.size() != latencies_ns.size()) {
        throw std::logic_error("threaded pread completion batch size mismatch");
    }
    const std::uint64_t dispatch_start = now_ns();
    for (const Task& task : tasks) {
        copy_completed_result(*task.submission, task.index);
    }

    std::vector<std::pair<std::shared_ptr<SubmissionState>, CompletionCallback>>
        completions;
    {
        std::lock_guard lock(queue_mutex_);
        for (std::size_t index = 0; index < tasks.size(); ++index) {
            const Task& task = tasks[index];
            const std::uint64_t latency_ns = latencies_ns[index];
            --in_flight_;
            --task.submission->in_flight;
            ++task.submission->completed;
            task.submission->total_latency_ns += latency_ns;
            ++tasks_completed_;
            queue_wait_ns_ += now_ns() - task.available_at_ns - latency_ns;
            execution_ns_ += latency_ns;
            if (task.submission->completed == task.submission->reads.size()) {
                task.submission->elapsed_ns = now_ns() - task.submission->available_at_ns;
                completions.emplace_back(task.submission,
                                         task.submission->on_complete);
            }
        }
    }

    for (const auto& completion : completions) {
        const auto& submission = completion.first;
        const auto& on_complete = completion.second;
        if (on_complete) {
            on_complete(submission);
        }
        submission->mark_complete();
    }

    {
        std::lock_guard lock(queue_mutex_);
        completion_dispatch_ns_ += now_ns() - dispatch_start;
    }
    queue_cv_.notify_one();
}

void ThreadedPreadBackend::worker_loop() {
    for (;;) {
        std::vector<Task> tasks;
        std::vector<std::uint64_t> latencies_ns;
        tasks.reserve(batch_size_);
        latencies_ns.reserve(batch_size_);
        {
            std::unique_lock lock(queue_mutex_);
            for (;;) {
                ++workers_idle_;
                queue_cv_.wait(lock, [this] {
                    return (stopping_ && !has_work_locked() && in_flight_ == 0) ||
                           (in_flight_ < max_in_flight_ && has_work_locked());
                });
                ++worker_wakeups_;
                if (stopping_ && !has_work_locked() && in_flight_ == 0) {
                    return;
                }
                if (in_flight_ >= max_in_flight_ || !has_work_locked()) {
                    ++spurious_wakeups_;
                    continue;
                }
                while (tasks.size() < batch_size_ &&
                       in_flight_ < max_in_flight_ && has_work_locked()) {
                    tasks.push_back(next_task_locked());
                    ++workers_busy_;
                }
                break;
            }
        }

        for (const Task& task : tasks) {
            const std::uint64_t start = now_ns();
            execute_pread(task.submission->reads[task.index]);
            const std::uint64_t stop = now_ns();
            latencies_ns.push_back(stop - start);
        }
        finish_tasks(tasks, latencies_ns);
    }
}

ExecutorMetricsSnapshot ThreadedPreadBackend::metrics() const noexcept {
    std::lock_guard lock(queue_mutex_);
    std::uint64_t queue_depth = 0;
    for (const auto& submission : active_) {
        queue_depth += static_cast<std::uint64_t>(
            submission->reads.size() - submission->next_index);
    }
    return ExecutorMetricsSnapshot{
        tasks_submitted_,
        tasks_completed_,
        queue_depth,
        queue_wait_ns_,
        execution_ns_,
        completion_dispatch_ns_,
        worker_wakeups_,
        spurious_wakeups_,
        workers_busy_,
        workers_idle_,
        max_queue_depth_,
        peak_in_flight_,
    };
}

} // namespace intentio::detail
