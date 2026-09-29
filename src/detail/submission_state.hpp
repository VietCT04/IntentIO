#pragma once

#include "physical_read.hpp"
#include "intentio/hints.hpp"
#include "intentio/result.hpp"

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

namespace intentio::detail {

struct SubmissionState {
    PhysicalPlan plan;
    std::vector<PhysicalRead> reads;
    std::vector<ReadResult> results;
    GroupHints hints{};
    std::size_t next_index{};
    std::size_t in_flight{};
    std::size_t completed{};
    std::size_t peak_in_flight{};
    std::uint64_t total_latency_ns{};
    std::uint64_t elapsed_ns{};
    std::uint64_t available_at_ns{};
    bool adaptive_enabled{false};
    std::function<void(const std::shared_ptr<SubmissionState>&)> on_complete;

    mutable std::mutex completion_mutex;
    mutable std::condition_variable completion_cv;
    bool complete{false};

    void mark_complete();
    void wait() const;
    [[nodiscard]] bool ready() const noexcept;
    [[nodiscard]] std::vector<ReadResult> copy_results() const;
};

} // namespace intentio::detail
