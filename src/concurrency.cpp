#include "detail/concurrency.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace intentio::detail {

namespace {

bool same_policy(const intentio::ConcurrencyPolicy& lhs,
                 const intentio::ConcurrencyPolicy& rhs) {
    return lhs.min_in_flight == rhs.min_in_flight &&
           lhs.max_in_flight == rhs.max_in_flight &&
           lhs.latency_growth_limit == rhs.latency_growth_limit &&
           lhs.throughput_improvement_threshold ==
               rhs.throughput_improvement_threshold &&
           lhs.observation_window == rhs.observation_window &&
           lhs.cooldown_windows == rhs.cooldown_windows;
}

} // namespace

ConcurrencyController::ConcurrencyController(
    const intentio::ConcurrencyPolicy& policy) {
    configure(policy);
}

void ConcurrencyController::configure(
    const intentio::ConcurrencyPolicy& requested_policy) {
    intentio::ConcurrencyPolicy policy = requested_policy;
    policy.min_in_flight = std::max<std::size_t>(1, policy.min_in_flight);
    policy.max_in_flight = std::max(policy.min_in_flight, policy.max_in_flight);
    policy.observation_window = std::max<std::size_t>(1, policy.observation_window);
    policy.latency_growth_limit =
        std::isfinite(policy.latency_growth_limit)
            ? std::max(1.0, policy.latency_growth_limit)
            : 1.0;
    policy.throughput_improvement_threshold =
        std::isfinite(policy.throughput_improvement_threshold)
            ? std::max(0.0, policy.throughput_improvement_threshold)
            : 0.0;

    if (same_policy(policy_, policy)) {
        return;
    }

    policy_ = policy;
    current_limit_ = std::clamp(current_limit_,
                                policy_.min_in_flight,
                                policy_.max_in_flight);
    peak_limit_ = std::max(peak_limit_, current_limit_);
    cooldown_remaining_ = 0;
    window_completed_ = 0;
    window_elapsed_ns_ = 0;
    window_latency_ns_ = 0;
    window_had_sufficient_work_ = false;
    have_baseline_ = false;
    baseline_rate_ = 0.0;
    baseline_latency_ns_ = 0.0;
}

std::size_t ConcurrencyController::current_limit() const noexcept {
    return current_limit_;
}

void ConcurrencyController::observe(const ExecutionObservation& observation) {
    if (observation.completed == 0 || observation.elapsed_ns == 0) {
        return;
    }

    const double average_latency =
        static_cast<double>(observation.total_latency_ns) /
        static_cast<double>(observation.completed);
    const double completion_rate =
        static_cast<double>(observation.completed) * 1.0e9 /
        static_cast<double>(observation.elapsed_ns);
    constexpr double alpha = 0.25;
    if (ewma_latency_ns_ == 0.0) {
        ewma_latency_ns_ = average_latency;
        ewma_completion_rate_ = completion_rate;
    } else {
        ewma_latency_ns_ = alpha * average_latency +
                           (1.0 - alpha) * ewma_latency_ns_;
        ewma_completion_rate_ = alpha * completion_rate +
                                (1.0 - alpha) * ewma_completion_rate_;
    }

    window_completed_ += observation.completed;
    window_elapsed_ns_ += observation.elapsed_ns;
    window_latency_ns_ += observation.total_latency_ns;
    window_had_sufficient_work_ =
        window_had_sufficient_work_ ||
        observation.available_work >= current_limit_;
    if (window_completed_ < policy_.observation_window) {
        return;
    }

    ++windows_;
    if (window_had_sufficient_work_ && window_elapsed_ns_ != 0) {
        const double window_rate =
            static_cast<double>(window_completed_) * 1.0e9 /
            static_cast<double>(window_elapsed_ns_);
        const double window_latency =
            static_cast<double>(window_latency_ns_) /
            static_cast<double>(window_completed_);

        if (!have_baseline_) {
            baseline_rate_ = window_rate;
            baseline_latency_ns_ = window_latency;
            have_baseline_ = true;
            if (current_limit_ < policy_.max_in_flight) {
                ++current_limit_;
                ++increases_;
                peak_limit_ = std::max(peak_limit_, current_limit_);
            }
        } else {
            const bool latency_bad =
                window_latency > baseline_latency_ns_ * policy_.latency_growth_limit;
            const bool throughput_bad =
                window_rate < baseline_rate_ *
                                   (1.0 - policy_.throughput_improvement_threshold);
            if ((latency_bad || throughput_bad) &&
                current_limit_ > policy_.min_in_flight) {
                const std::size_t next_limit = std::max(
                    policy_.min_in_flight,
                    std::max<std::size_t>(1, current_limit_ / 2));
                if (next_limit < current_limit_) {
                    current_limit_ = next_limit;
                    ++decreases_;
                    cooldown_remaining_ = policy_.cooldown_windows;
                }
            } else if (cooldown_remaining_ != 0) {
                --cooldown_remaining_;
            } else if (current_limit_ < policy_.max_in_flight) {
                ++current_limit_;
                ++increases_;
                peak_limit_ = std::max(peak_limit_, current_limit_);
            }

            baseline_rate_ = std::max(baseline_rate_, window_rate);
            baseline_latency_ns_ =
                std::min(baseline_latency_ns_, window_latency);
        }
    }

    window_completed_ = 0;
    window_elapsed_ns_ = 0;
    window_latency_ns_ = 0;
    window_had_sufficient_work_ = false;
}

ConcurrencySnapshot ConcurrencyController::snapshot() const noexcept {
    return ConcurrencySnapshot{
        current_limit_,
        peak_limit_,
        increases_,
        decreases_,
        windows_,
        ewma_latency_ns_ / 1.0e6,
        ewma_completion_rate_,
    };
}

} // namespace intentio::detail
