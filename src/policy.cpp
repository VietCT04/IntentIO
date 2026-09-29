#include "detail/policy.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace intentio::detail {

namespace {

void make_conservative(PlanningPolicy& policy,
                       const PlanningPolicy& conservative) {
    policy.max_merge_gap_bytes =
        std::min(policy.max_merge_gap_bytes, conservative.max_merge_gap_bytes);
    policy.max_merged_size_bytes =
        std::min(policy.max_merged_size_bytes, conservative.max_merged_size_bytes);
    policy.max_read_amplification =
        std::min(policy.max_read_amplification,
                 conservative.max_read_amplification);
}

void make_more_aggressive(PlanningPolicy& policy,
                          const PlanningPolicy& aggressive) {
    policy.max_merge_gap_bytes =
        std::max(policy.max_merge_gap_bytes, aggressive.max_merge_gap_bytes);
    policy.max_merged_size_bytes =
        std::max(policy.max_merged_size_bytes,
                 aggressive.max_merged_size_bytes);
    policy.max_read_amplification =
        std::max(policy.max_read_amplification,
                 aggressive.max_read_amplification);
}

} // namespace

PlanningPolicy resolve_policy(const RuntimeOptions& options,
                              const GroupHints& hints) {
    PlanningPolicy policy = options.intent_policy.unknown;

    if (hints.latency_class == LatencyClass::LatencySensitive) {
        policy = options.intent_policy.latency_sensitive;
    } else if (hints.latency_class == LatencyClass::Throughput) {
        policy = options.intent_policy.throughput;
        if (hints.access_pattern == AccessPattern::Sequential) {
            make_more_aggressive(policy, options.intent_policy.sequential);
        } else if (hints.access_pattern == AccessPattern::Random) {
            make_conservative(policy, options.intent_policy.random);
        }
    } else if (hints.access_pattern == AccessPattern::Sequential) {
        policy = options.intent_policy.sequential;
    } else if (hints.access_pattern == AccessPattern::Random) {
        policy = options.intent_policy.random;
    }

    if (hints.reuse_hint == ReuseHint::ScanOnce) {
        make_more_aggressive(policy, options.intent_policy.throughput);
    } else if (hints.reuse_hint == ReuseHint::ReuseLikely) {
        make_conservative(policy, options.intent_policy.unknown);
    }

    // Existing runtime knobs remain hard global safety caps. They can only
    // restrict an intent policy, never authorize more aggressive behavior.
    const auto global_gap_cap =
        options.coalescing.max_gap_bytes >
                static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())
            ? std::numeric_limits<std::size_t>::max()
            : static_cast<std::size_t>(options.coalescing.max_gap_bytes);
    policy.max_merge_gap_bytes =
        std::min(policy.max_merge_gap_bytes, global_gap_cap);
    if (options.coalescing.max_request_bytes != 0) {
        policy.max_merged_size_bytes =
            std::min(policy.max_merged_size_bytes,
                     options.coalescing.max_request_bytes);
    }
    policy.enable_coalescing =
        policy.enable_coalescing && options.coalescing.enabled;
    policy.enable_deduplication =
        policy.enable_deduplication && options.deduplication.enabled;

    if (!std::isfinite(policy.max_read_amplification) ||
        policy.max_read_amplification < 1.0) {
        policy.max_read_amplification = 1.0;
    }
    return policy;
}

} // namespace intentio::detail
