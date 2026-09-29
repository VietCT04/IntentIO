#pragma once

#include <cstddef>

namespace intentio {

// Backend-independent fixed execution limit. Backends must never have more
// than this many physical operations outstanding for one plan.
struct ExecutionPolicy {
    std::size_t max_in_flight{32};
};

// Simple AIMD controller parameters. The controller is intentionally separate
// from PlanningPolicy: this policy affects scheduling, not range shape.
struct ConcurrencyPolicy {
    std::size_t min_in_flight{1};
    std::size_t max_in_flight{32};
    double latency_growth_limit{1.50};
    double throughput_improvement_threshold{0.05};
    std::size_t observation_window{16};
    std::size_t cooldown_windows{1};
};

struct IntentExecutionPolicyOptions {
    ConcurrencyPolicy unknown{};
    ConcurrencyPolicy sequential{1, 64, 1.50, 0.05, 16, 1};
    ConcurrencyPolicy random{1, 32, 1.35, 0.05, 16, 1};
    ConcurrencyPolicy latency_sensitive{1, 8, 1.15, 0.10, 16, 2};
    ConcurrencyPolicy throughput{1, 128, 1.75, 0.03, 16, 1};
};

struct AdaptiveExecutionOptions {
    bool enabled{false};
    IntentExecutionPolicyOptions intent{};
};

} // namespace intentio
