#include "intentio/runtime.hpp"

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

} // namespace

RuntimeState::RuntimeState(const RuntimeOptions& requested_options)
    : options(requested_options), selected_backend(Backend::Synchronous) {
    if (options.queue_depth == 0) {
        throw std::invalid_argument("IntentIO queue depth must be greater than zero");
    }
    const char* trace_environment = std::getenv("INTENTIO_TRACE_PLAN");
    trace_plan = options.trace_plan;
    trace_plan = trace_plan ||
                 (trace_environment != nullptr && trace_environment[0] == '1');

    switch (options.backend) {
    case Backend::Synchronous:
        selected_backend = Backend::Synchronous;
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
            selected_backend = Backend::Synchronous;
        }
        break;
    }
}

RuntimeState::~RuntimeState() = default;

std::vector<ReadResult> RuntimeState::submit(
    const std::vector<ReadRequest>& requests,
    const GroupHints& hints) {
    std::lock_guard lock(mutex);

    const PlanningPolicy policy = resolve_policy(options, hints);

    std::vector<ReadResult> results;
    results.reserve(requests.size());
    for (const ReadRequest& request : requests) {
        results.push_back(ReadResult{
            request.id,
            request.offset,
            request.length,
            {},
            {},
        });
    }

    const PhysicalPlan plan = plan_reads(
        requests,
        policy,
        PlannerOptions{trace_plan});

    if (trace_plan) {
        std::clog << "IntentIO plan\n"
                  << "  Intent: access_pattern="
                  << access_pattern_name(hints.access_pattern)
                  << " reuse_hint=" << reuse_hint_name(hints.reuse_hint)
                  << " latency_class=" << latency_class_name(hints.latency_class)
                  << "\n"
                  << "  Resolved policy: max_merge_gap="
                  << policy.max_merge_gap_bytes
                  << " max_merged_size=" << policy.max_merged_size_bytes
                  << " max_read_amplification="
                  << policy.max_read_amplification
                  << " enable_coalescing=" << policy.enable_coalescing
                  << " enable_deduplication=" << policy.enable_deduplication
                  << "\n"
                  << plan.trace;
    }

    std::vector<PhysicalRead> reads;
    reads.reserve(plan.requests.size());
    for (const PhysicalRequest& request : plan.requests) {
        PhysicalRead read;
        read.request = request;
        read.data.resize(request.length);
        reads.push_back(std::move(read));
    }

    if (selected_backend == Backend::IoUring) {
        io_uring_backend->execute(reads);
    } else {
        sync_backend.execute(reads);
    }

    for (const PhysicalRead& read : reads) {
        for (const PhysicalSlice& slice : read.request.slices) {
            ReadResult& result = results[slice.logical_index];
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

    metrics.logical_reads += requests.size();
    for (const ReadRequest& request : requests) {
        metrics.logical_bytes += request.length;
    }
    metrics.unique_logical_bytes += plan.unique_logical_bytes;
    metrics.physical_reads += plan.requests.size();
    for (const PhysicalRequest& request : plan.requests) {
        metrics.physical_bytes += request.length;
    }
    metrics.requests_merged += plan.requests_merged;
    metrics.merge_saved_operations += plan.merge_saved_operations;
    metrics.requests_deduplicated += plan.requests_deduplicated;
    metrics.physical_reads_avoided_by_dedup +=
        plan.physical_reads_avoided_by_dedup;
    metrics.bytes_avoided_by_dedup += plan.bytes_avoided_by_dedup;
    metrics.merge_candidates += plan.merge_candidates;
    metrics.merges_accepted += plan.merges_accepted;
    metrics.merges_rejected += plan.merges_rejected;
    metrics.merges_rejected_amplification +=
        plan.merges_rejected_amplification;
    metrics.merges_rejected_size += plan.merges_rejected_size;
    metrics.merges_rejected_gap += plan.merges_rejected_gap;
    metrics.merges_rejected_policy += plan.merges_rejected_policy;
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
    metrics.read_amplification =
        metrics.unique_logical_bytes == 0
            ? 0.0
            : static_cast<double>(metrics.physical_bytes) /
                  static_cast<double>(metrics.unique_logical_bytes);

    return results;
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

Backend Runtime::backend() const noexcept {
    return state_ ? state_->selected_backend : Backend::Synchronous;
}

MetricsSnapshot Runtime::metrics() const {
    if (!state_) {
        return {};
    }
    std::lock_guard lock(state_->mutex);
    return state_->metrics;
}

} // namespace intentio
