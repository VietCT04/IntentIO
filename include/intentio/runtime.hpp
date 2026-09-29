#pragma once

#include "group.hpp"
#include "metrics.hpp"
#include "policy.hpp"
#include "execution.hpp"
#include "submission.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>

namespace intentio {

enum class Backend {
    Synchronous,
    ThreadedPread,
    IoUring,
    Auto,
};

struct CoalescingOptions {
    bool enabled{true};
    // This is a hard global cap. Intent policies may choose a smaller gap.
    std::uint64_t max_gap_bytes{64U * 1024U};
    std::size_t max_request_bytes{8U * 1024U * 1024U};
};

struct DeduplicationOptions {
    bool enabled{true};
};

struct RuntimeOptions {
    Backend backend{Backend::Synchronous};
    std::size_t queue_depth{32};
    CoalescingOptions coalescing{};
    DeduplicationOptions deduplication{};
    IntentPolicyOptions intent_policy{};
    ExecutionPolicy execution{};
    AdaptiveExecutionOptions adaptive_execution{};
    std::size_t threaded_worker_count{32};
    std::size_t executor_batch_size{1};
    bool trace_plan{false};
};

class Runtime {
public:
    explicit Runtime(RuntimeOptions options = {});
    Runtime(Runtime&&) noexcept;
    Runtime& operator=(Runtime&&) noexcept;
    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;
    ~Runtime();

    [[nodiscard]] Group create_group();
    [[nodiscard]] Submission submit(Group& group);
    [[nodiscard]] Submission submit(Group&& group) { return submit(group); }
    [[nodiscard]] Backend backend() const noexcept;
    [[nodiscard]] MetricsSnapshot metrics() const;

private:
    std::shared_ptr<detail::RuntimeState> state_;
};

} // namespace intentio
