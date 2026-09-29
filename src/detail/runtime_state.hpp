#pragma once

#include "backends.hpp"
#include "concurrency.hpp"
#include "intentio/metrics.hpp"
#include "intentio/result.hpp"
#include "intentio/runtime.hpp"

#include <memory>
#include <mutex>
#include <vector>

namespace intentio::detail {

struct RuntimeState : std::enable_shared_from_this<RuntimeState> {
    explicit RuntimeState(const RuntimeOptions& options);
    ~RuntimeState();

    RuntimeState(const RuntimeState&) = delete;
    RuntimeState& operator=(const RuntimeState&) = delete;

    [[nodiscard]] std::vector<ReadResult> submit(
        const std::vector<ReadRequest>& requests,
        const GroupHints& hints);
    [[nodiscard]] std::shared_ptr<SubmissionState> submit_async(
        const std::vector<ReadRequest>& requests,
        const GroupHints& hints);
    [[nodiscard]] MetricsSnapshot metrics_snapshot() const;

    RuntimeOptions options;
    Backend selected_backend{Backend::Synchronous};
    SyncBackend sync_backend;
    std::shared_ptr<ThreadedPreadBackend> threaded_backend;
    std::unique_ptr<IoUringBackend> io_uring_backend;
    ConcurrencyController concurrency_controller;
    MetricsSnapshot metrics{};
    bool trace_plan{false};
    mutable std::mutex metrics_mutex;
    mutable std::mutex controller_mutex;
    mutable std::mutex blocking_backend_mutex;
    mutable std::mutex trace_mutex;

private:
    void complete_submission(const std::shared_ptr<SubmissionState>& submission);
};

} // namespace intentio::detail
