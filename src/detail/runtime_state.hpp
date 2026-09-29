#pragma once

#include "backends.hpp"
#include "intentio/metrics.hpp"
#include "intentio/result.hpp"
#include "intentio/runtime.hpp"

#include <memory>
#include <mutex>
#include <vector>

namespace intentio::detail {

struct RuntimeState {
    explicit RuntimeState(const RuntimeOptions& options);
    ~RuntimeState();

    RuntimeState(const RuntimeState&) = delete;
    RuntimeState& operator=(const RuntimeState&) = delete;

    [[nodiscard]] std::vector<ReadResult> submit(
        const std::vector<ReadRequest>& requests,
        const GroupHints& hints);

    RuntimeOptions options;
    Backend selected_backend{Backend::Synchronous};
    SyncBackend sync_backend;
    std::unique_ptr<IoUringBackend> io_uring_backend;
    MetricsSnapshot metrics{};
    bool trace_plan{false};
    mutable std::mutex mutex;
};

} // namespace intentio::detail
