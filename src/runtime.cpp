#include "intentio/runtime.hpp"

#include "detail/execution_policy.hpp"
#include "detail/policy.hpp"
#include "detail/runtime_state.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <system_error>

namespace intentio::detail {

namespace {

const char* access_pattern_name(AccessPattern pattern) {
    switch (pattern) {
    case AccessPattern::Unknown:
        return "Unknown";
    case AccessPattern::Sequential:
        return "Sequential";
    case AccessPattern::Random:
        return "Random";
    }
    return "Unknown";
}

const char* reuse_hint_name(ReuseHint hint) {
    switch (hint) {
    case ReuseHint::Unknown:
        return "Unknown";
    case ReuseHint::ReuseLikely:
        return "ReuseLikely";
    case ReuseHint::ScanOnce:
        return "ScanOnce";
    }
    return "Unknown";
}

const char* latency_class_name(LatencyClass latency) {
    switch (latency) {
    case LatencyClass::Default:
        return "Default";
    case LatencyClass::LatencySensitive:
        return "LatencySensitive";
    case LatencyClass::Throughput:
        return "Throughput";
    }
    return "Default";
}

void add_kernel_metrics(MetricsSnapshot& metrics,
                        const KernelIOStats& stats) noexcept {
    metrics.kernel_advice_sequential += stats.kernel_advice_sequential;
    metrics.kernel_advice_random += stats.kernel_advice_random;
    metrics.kernel_advice_noreuse += stats.kernel_advice_noreuse;
    metrics.kernel_advice_failures += stats.kernel_advice_failures;
    metrics.dontcache_reads_attempted += stats.dontcache_reads_attempted;
    metrics.dontcache_reads_succeeded += stats.dontcache_reads_succeeded;
    metrics.dontcache_unsupported += stats.dontcache_unsupported;
    metrics.dontcache_fallbacks += stats.dontcache_fallbacks;
    metrics.buffered_reads += stats.buffered_reads;
    metrics.bytes_dontcache += stats.bytes_dontcache;
    metrics.bytes_buffered += stats.bytes_buffered;
}

} // namespace

RuntimeState::RuntimeState(const RuntimeOptions& requested_options)
    : options(requested_options), selected_backend(Backend::Synchronous) {
    if (options.queue_depth == 0) {
        throw std::invalid_argument("IntentIO queue depth must be greater than zero");
    }
    if (options.execution.max_in_flight == 0) {
        throw std::invalid_argument("IntentIO max in-flight must be greater than zero");
    }
    if (options.threaded_worker_count == 0) {
        throw std::invalid_argument("IntentIO threaded worker count must be greater than zero");
    }
    if (options.executor_batch_size == 0) {
        throw std::invalid_argument("IntentIO executor batch size must be greater than zero");
    }
    const char* trace_environment = std::getenv("INTENTIO_TRACE_PLAN");
    trace_plan = options.trace_plan;
    trace_plan = trace_plan ||
                 (trace_environment != nullptr && trace_environment[0] == '1');

    switch (options.backend) {
    case Backend::Synchronous:
        selected_backend = Backend::Synchronous;
        break;
    case Backend::ThreadedPread:
        threaded_backend = std::make_shared<ThreadedPreadBackend>(
            options.threaded_worker_count,
            options.execution.max_in_flight,
            options.executor_batch_size);
        selected_backend = Backend::ThreadedPread;
        break;
    case Backend::IoUring:
        io_uring_backend = std::make_unique<IoUringBackend>(options.queue_depth);
        selected_backend = Backend::IoUring;
        break;
    case Backend::Auto:
        try {
            io_uring_backend = std::make_unique<IoUringBackend>(options.queue_depth);
            selected_backend = Backend::IoUring;
        } catch (const std::system_error&) {
            threaded_backend = std::make_shared<ThreadedPreadBackend>(
                options.threaded_worker_count,
                options.execution.max_in_flight,
                options.executor_batch_size);
            selected_backend = Backend::ThreadedPread;
        }
        break;
    }
}

RuntimeState::~RuntimeState() = default;

std::shared_ptr<SubmissionState> RuntimeState::submit_async(
    const std::vector<ReadRequest>& requests,
    const GroupHints& hints) {
    const PlanningPolicy planning_policy = resolve_policy(options, hints);
    const ResolvedExecutionPolicy resolved_execution =
        resolve_execution_policy(options, hints);
    if (resolved_execution.adaptive_enabled) {
        std::lock_guard lock(controller_mutex);
        concurrency_controller.configure(resolved_execution.adaptive);
    }

    auto submission = std::make_shared<SubmissionState>();
    submission->hints = hints;
    submission->kernel_io_policy =
        resolve_kernel_io_policy(options.kernel_io, hints);
    submission->results.reserve(requests.size());
    for (const ReadRequest& request : requests) {
        submission->results.push_back(ReadResult{
            request.id,
            request.offset,
            request.length,
            {},
            {},
        });
    }

    PhysicalPlan plan = plan_reads(
        requests,
        planning_policy,
        PlannerOptions{trace_plan});
    submission->plan = std::move(plan);
    submission->reads.reserve(submission->plan.requests.size());
    for (const PhysicalRequest& request : submission->plan.requests) {
        PhysicalRead read;
        read.request = request;
        read.data.resize(request.length);
        submission->reads.push_back(std::move(read));
    }

    ExecutionPolicy execution_policy = resolved_execution.fixed;
    if (resolved_execution.adaptive_enabled) {
        std::lock_guard lock(controller_mutex);
        execution_policy.max_in_flight = concurrency_controller.current_limit();
    }
    submission->adaptive_enabled = resolved_execution.adaptive_enabled;

    if (trace_plan) {
        std::lock_guard trace_lock(trace_mutex);
        std::clog << "IntentIO plan\n"
                  << "  Intent: access_pattern="
                  << access_pattern_name(hints.access_pattern)
                  << " reuse_hint=" << reuse_hint_name(hints.reuse_hint)
                  << " latency_class=" << latency_class_name(hints.latency_class)
                  << "\n"
                  << "  Resolved policy: max_merge_gap="
                  << planning_policy.max_merge_gap_bytes
                  << " max_merged_size=" << planning_policy.max_merged_size_bytes
                  << " max_read_amplification="
                  << planning_policy.max_read_amplification
                  << " enable_coalescing=" << planning_policy.enable_coalescing
                  << " enable_deduplication="
                  << planning_policy.enable_deduplication
                  << "\n"
                  << "  Execution policy: max_in_flight="
                  << execution_policy.max_in_flight
                  << " adaptive=" << resolved_execution.adaptive_enabled
                  << "\n"
                  << "  Kernel I/O policy: advice="
                  << file_advice_name(submission->kernel_io_policy.access_advice)
                  << " cache="
                  << cache_policy_name(submission->kernel_io_policy.cache_policy)
                  << "\n"
                  << submission->plan.trace;
    }

    {
        std::lock_guard metrics_lock(metrics_mutex);
        metrics.logical_reads += requests.size();
        for (const ReadRequest& request : requests) {
            metrics.logical_bytes += request.length;
        }
        metrics.unique_logical_bytes += submission->plan.unique_logical_bytes;
        metrics.physical_reads += submission->plan.requests.size();
        for (const PhysicalRequest& request : submission->plan.requests) {
            metrics.physical_bytes += request.length;
        }
        metrics.requests_merged += submission->plan.requests_merged;
        metrics.merge_saved_operations += submission->plan.merge_saved_operations;
        metrics.requests_deduplicated += submission->plan.requests_deduplicated;
        metrics.physical_reads_avoided_by_dedup +=
            submission->plan.physical_reads_avoided_by_dedup;
        metrics.bytes_avoided_by_dedup += submission->plan.bytes_avoided_by_dedup;
        metrics.merge_candidates += submission->plan.merge_candidates;
        metrics.merges_accepted += submission->plan.merges_accepted;
        metrics.merges_rejected += submission->plan.merges_rejected;
        metrics.merges_rejected_amplification +=
            submission->plan.merges_rejected_amplification;
        metrics.merges_rejected_size += submission->plan.merges_rejected_size;
        metrics.merges_rejected_gap += submission->plan.merges_rejected_gap;
        metrics.merges_rejected_policy += submission->plan.merges_rejected_policy;
        if (hints.access_pattern == AccessPattern::Sequential) {
            ++metrics.intent_sequential_groups;
        } else if (hints.access_pattern == AccessPattern::Random) {
            ++metrics.intent_random_groups;
        }
        if (hints.latency_class == LatencyClass::LatencySensitive) {
            ++metrics.latency_sensitive_groups;
        } else if (hints.latency_class == LatencyClass::Throughput) {
            ++metrics.throughput_groups;
        }
        metrics.read_amplification = metrics.unique_logical_bytes == 0
                                         ? 0.0
                                         : static_cast<double>(metrics.physical_bytes) /
                                               static_cast<double>(metrics.unique_logical_bytes);
        metrics.current_concurrency = selected_backend == Backend::Synchronous
                                          ? 1
                                          : execution_policy.max_in_flight;
    }

    const std::weak_ptr<RuntimeState> weak_state = shared_from_this();
    const auto on_complete = [weak_state](
                                  const std::shared_ptr<SubmissionState>& completed) {
        if (const auto state = weak_state.lock()) {
            state->complete_submission(completed);
        }
    };

    if (selected_backend == Backend::ThreadedPread) {
        threaded_backend->set_max_in_flight(execution_policy.max_in_flight);
        threaded_backend->submit(submission, on_complete);
        return submission;
    }

    std::lock_guard backend_lock(blocking_backend_mutex);
    ExecutionStats stats;
    if (selected_backend == Backend::IoUring) {
        stats = io_uring_backend->execute(submission->reads, execution_policy);
        submission->completed = stats.completed;
        submission->peak_in_flight = stats.peak_in_flight;
        submission->elapsed_ns = stats.elapsed_ns;
        submission->total_latency_ns = stats.total_latency_ns;
    } else {
        stats = sync_backend.execute(submission->reads,
                                     execution_policy,
                                     submission->kernel_io_policy);
        submission->completed = stats.completed;
        submission->peak_in_flight = stats.peak_in_flight;
        submission->elapsed_ns = stats.elapsed_ns;
        submission->total_latency_ns = stats.total_latency_ns;
    }
    {
        std::lock_guard metrics_lock(metrics_mutex);
        add_kernel_metrics(metrics, stats.kernel_io);
    }
    for (std::size_t index = 0; index < submission->reads.size(); ++index) {
        const PhysicalRead& read = submission->reads[index];
        for (const PhysicalSlice& slice : read.request.slices) {
            ReadResult& result = submission->results[slice.logical_index];
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
    on_complete(submission);
    submission->mark_complete();
    return submission;
}

void RuntimeState::complete_submission(
    const std::shared_ptr<SubmissionState>& submission) {
    if (submission->adaptive_enabled && selected_backend != Backend::Synchronous) {
        std::size_t next_limit = options.execution.max_in_flight;
        {
            std::lock_guard controller_lock(controller_mutex);
            concurrency_controller.observe(ExecutionObservation{
                submission->completed,
                submission->plan.requests.size(),
                submission->peak_in_flight,
                submission->elapsed_ns,
                submission->total_latency_ns,
            });
            next_limit = concurrency_controller.current_limit();
        }
        if (selected_backend == Backend::ThreadedPread) {
            threaded_backend->set_max_in_flight(next_limit);
        }
    }

    const ExecutionStats stats{
        submission->completed,
        submission->peak_in_flight,
        submission->elapsed_ns,
        submission->total_latency_ns,
    };
    const auto concurrency = [&] {
        std::lock_guard controller_lock(controller_mutex);
        return concurrency_controller.snapshot();
    }();
    {
        std::lock_guard metrics_lock(metrics_mutex);
        metrics.concurrency_increases = concurrency.increases;
        metrics.concurrency_decreases = concurrency.decreases;
        metrics.controller_windows = concurrency.windows;
        metrics.ewma_completion_latency_ms = concurrency.ewma_latency_ms;
        metrics.completion_rate = concurrency.completion_rate;
        metrics.peak_concurrency = std::max(metrics.peak_concurrency,
                                            stats.peak_in_flight);
        if (submission->adaptive_enabled) {
            metrics.current_concurrency = concurrency.current_limit;
            metrics.peak_concurrency = std::max(metrics.peak_concurrency,
                                                concurrency.peak_limit);
        }
    }
}

std::vector<ReadResult> RuntimeState::submit(
    const std::vector<ReadRequest>& requests,
    const GroupHints& hints) {
    return submit_async(requests, hints)->copy_results();
}

MetricsSnapshot RuntimeState::metrics_snapshot() const {
    MetricsSnapshot snapshot;
    {
        std::lock_guard metrics_lock(metrics_mutex);
        snapshot = metrics;
    }
    if (threaded_backend) {
        const ExecutorMetricsSnapshot executor = threaded_backend->metrics();
        snapshot.executor_tasks_submitted = executor.tasks_submitted;
        snapshot.executor_tasks_completed = executor.tasks_completed;
        snapshot.executor_queue_depth = executor.queue_depth;
        snapshot.executor_queue_wait_ns = executor.queue_wait_ns;
        snapshot.executor_execution_ns = executor.execution_ns;
        snapshot.executor_completion_dispatch_ns =
            executor.completion_dispatch_ns;
        snapshot.executor_worker_wakeups = executor.worker_wakeups;
        snapshot.executor_spurious_wakeups = executor.spurious_wakeups;
        snapshot.executor_workers_busy = executor.workers_busy;
        snapshot.executor_workers_idle = executor.workers_idle;
        snapshot.executor_max_queue_depth = executor.max_queue_depth;
        snapshot.executor_peak_in_flight = executor.peak_in_flight;
        snapshot.kernel_advice_sequential =
            executor.kernel_io.kernel_advice_sequential;
        snapshot.kernel_advice_random = executor.kernel_io.kernel_advice_random;
        snapshot.kernel_advice_noreuse = executor.kernel_io.kernel_advice_noreuse;
        snapshot.kernel_advice_failures = executor.kernel_io.kernel_advice_failures;
        snapshot.dontcache_reads_attempted =
            executor.kernel_io.dontcache_reads_attempted;
        snapshot.dontcache_reads_succeeded =
            executor.kernel_io.dontcache_reads_succeeded;
        snapshot.dontcache_unsupported = executor.kernel_io.dontcache_unsupported;
        snapshot.dontcache_fallbacks = executor.kernel_io.dontcache_fallbacks;
        snapshot.buffered_reads = executor.kernel_io.buffered_reads;
        snapshot.bytes_dontcache = executor.kernel_io.bytes_dontcache;
        snapshot.bytes_buffered = executor.kernel_io.bytes_buffered;
    }
    return snapshot;
}

} // namespace intentio::detail

namespace intentio {

Runtime::Runtime(RuntimeOptions options)
    : state_(std::make_shared<detail::RuntimeState>(options)) {}

Runtime::Runtime(Runtime&&) noexcept = default;

Runtime& Runtime::operator=(Runtime&&) noexcept = default;

Runtime::~Runtime() = default;

Group Runtime::create_group() {
    if (!state_) {
        throw std::logic_error("cannot create a group from a moved-from runtime");
    }
    return Group(state_);
}

Submission Runtime::submit(Group& group) {
    if (!state_) {
        throw std::logic_error("cannot submit through a moved-from runtime");
    }
    if (group.state_ != state_) {
        throw std::invalid_argument("group belongs to a different runtime");
    }
    if (group.submitted_) {
        throw std::logic_error("Group may only be submitted once");
    }
    group.submitted_ = true;
    auto submission_state = state_->submit_async(group.requests_, group.hints_);
    std::shared_ptr<void> keep_alive;
    if (state_->threaded_backend) {
        keep_alive = state_->threaded_backend;
    }
    return Submission(std::move(submission_state), std::move(keep_alive));
}

Backend Runtime::backend() const noexcept {
    return state_ ? state_->selected_backend : Backend::Synchronous;
}

MetricsSnapshot Runtime::metrics() const {
    if (!state_) {
        return {};
    }
    return state_->metrics_snapshot();
}

} // namespace intentio
