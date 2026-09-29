#pragma once

#include "intentio/execution.hpp"

#include <cstddef>
#include <cstdint>

namespace intentio::detail {

struct ExecutionObservation {
    std::size_t completed{};
    std::size_t available_work{};
    std::size_t peak_in_flight{};
    std::uint64_t elapsed_ns{};
    std::uint64_t total_latency_ns{};
};

struct ConcurrencySnapshot {
    std::size_t current_limit{1};
    std::size_t peak_limit{1};
    std::uint64_t increases{};
    std::uint64_t decreases{};
    std::uint64_t windows{};
    double ewma_latency_ms{};
    double completion_rate{};
};

class ConcurrencyController {
public:
    explicit ConcurrencyController(
        const intentio::ConcurrencyPolicy& policy = {});

    void configure(const intentio::ConcurrencyPolicy& policy);
    [[nodiscard]] std::size_t current_limit() const noexcept;
    void observe(const ExecutionObservation& observation);
    [[nodiscard]] ConcurrencySnapshot snapshot() const noexcept;

private:
    intentio::ConcurrencyPolicy policy_{};
    std::size_t current_limit_{1};
    std::size_t peak_limit_{1};
    std::uint64_t increases_{};
    std::uint64_t decreases_{};
    std::uint64_t windows_{};
    std::size_t cooldown_remaining_{};
    std::size_t window_completed_{};
    std::uint64_t window_elapsed_ns_{};
    std::uint64_t window_latency_ns_{};
    bool window_had_sufficient_work_{false};
    bool have_baseline_{false};
    double baseline_rate_{};
    double baseline_latency_ns_{};
    double ewma_latency_ns_{};
    double ewma_completion_rate_{};
};

} // namespace intentio::detail
