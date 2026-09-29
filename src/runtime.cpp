#include "intentio/runtime.hpp"

#include "detail/runtime_state.hpp"

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <system_error>

namespace intentio::detail {

RuntimeState::RuntimeState(const RuntimeOptions& requested_options)
    : options(requested_options), selected_backend(Backend::Synchronous) {
    if (options.queue_depth == 0) {
        throw std::invalid_argument("IntentIO queue depth must be greater than zero");
    }

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
        hints,
        PlannerOptions{
            options.coalescing.enabled,
            options.coalescing.max_gap_bytes,
            options.coalescing.max_request_bytes,
        });

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
    metrics.physical_reads += plan.requests.size();
    for (const PhysicalRequest& request : plan.requests) {
        metrics.physical_bytes += request.length;
        if (request.slices.size() > 1) {
            const auto saved = static_cast<std::uint64_t>(request.slices.size() - 1);
            metrics.requests_merged += saved;
            metrics.merge_saved_operations += saved;
        }
    }

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
