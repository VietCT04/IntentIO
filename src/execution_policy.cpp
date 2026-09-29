#include "detail/execution_policy.hpp"

#include <algorithm>
#include <cmath>

namespace intentio::detail {

namespace {

void make_conservative(intentio::ConcurrencyPolicy& policy,
                       const intentio::ConcurrencyPolicy& conservative) {
    policy.min_in_flight =
        std::max(policy.min_in_flight, conservative.min_in_flight);
    policy.max_in_flight =
        std::min(policy.max_in_flight, conservative.max_in_flight);
    policy.latency_growth_limit =
        std::min(policy.latency_growth_limit, conservative.latency_growth_limit);
    policy.throughput_improvement_threshold = std::max(
        policy.throughput_improvement_threshold,
        conservative.throughput_improvement_threshold);
    policy.observation_window =
        std::max(policy.observation_window, conservative.observation_window);
    policy.cooldown_windows =
        std::max(policy.cooldown_windows, conservative.cooldown_windows);
}

void make_more_aggressive(intentio::ConcurrencyPolicy& policy,
                          const intentio::ConcurrencyPolicy& aggressive) {
    policy.min_in_flight =
        std::min(policy.min_in_flight, aggressive.min_in_flight);
    policy.max_in_flight =
        std::max(policy.max_in_flight, aggressive.max_in_flight);
    policy.latency_growth_limit =
        std::max(policy.latency_growth_limit, aggressive.latency_growth_limit);
    policy.throughput_improvement_threshold = std::min(
        policy.throughput_improvement_threshold,
        aggressive.throughput_improvement_threshold);
    policy.observation_window =
        std::min(policy.observation_window, aggressive.observation_window);
    policy.cooldown_windows =
        std::min(policy.cooldown_windows, aggressive.cooldown_windows);
}

} // namespace

ResolvedExecutionPolicy resolve_execution_policy(
    const intentio::RuntimeOptions& options,
    const intentio::GroupHints& hints) {
    const auto& presets = options.adaptive_execution.intent;
    intentio::ConcurrencyPolicy adaptive = presets.unknown;

    if (hints.latency_class == intentio::LatencyClass::LatencySensitive) {
        adaptive = presets.latency_sensitive;
    } else if (hints.latency_class == intentio::LatencyClass::Throughput) {
        adaptive = presets.throughput;
        if (hints.access_pattern == intentio::AccessPattern::Random) {
            make_conservative(adaptive, presets.random);
        } else if (hints.access_pattern == intentio::AccessPattern::Sequential) {
            make_more_aggressive(adaptive, presets.sequential);
        }
    } else if (hints.access_pattern == intentio::AccessPattern::Sequential) {
        adaptive = presets.sequential;
    } else if (hints.access_pattern == intentio::AccessPattern::Random) {
        adaptive = presets.random;
    }

    if (hints.reuse_hint == intentio::ReuseHint::ScanOnce) {
        make_more_aggressive(adaptive, presets.throughput);
    } else if (hints.reuse_hint == intentio::ReuseHint::ReuseLikely) {
        make_conservative(adaptive, presets.unknown);
    }

    const std::size_t global_max = options.execution.max_in_flight;
    adaptive.max_in_flight = std::min(adaptive.max_in_flight, global_max);
    adaptive.min_in_flight = std::max<std::size_t>(1, adaptive.min_in_flight);
    adaptive.min_in_flight = std::min(adaptive.min_in_flight,
                                      adaptive.max_in_flight);
    adaptive.observation_window = std::max<std::size_t>(1,
                                                         adaptive.observation_window);
    if (!std::isfinite(adaptive.latency_growth_limit) ||
        adaptive.latency_growth_limit < 1.0) {
        adaptive.latency_growth_limit = 1.0;
    }
    if (!std::isfinite(adaptive.throughput_improvement_threshold) ||
        adaptive.throughput_improvement_threshold < 0.0) {
        adaptive.throughput_improvement_threshold = 0.0;
    }

    ResolvedExecutionPolicy resolved;
    resolved.fixed = options.execution;
    resolved.adaptive = adaptive;
    resolved.adaptive_enabled = options.adaptive_execution.enabled;
    return resolved;
}

} // namespace intentio::detail
