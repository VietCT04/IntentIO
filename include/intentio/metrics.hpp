#pragma once

#include <cstdint>

namespace intentio {

struct MetricsSnapshot {
    std::uint64_t logical_reads{};
    std::uint64_t physical_reads{};
    std::uint64_t logical_bytes{};
    std::uint64_t unique_logical_bytes{};
    std::uint64_t physical_bytes{};
    std::uint64_t requests_merged{};
    std::uint64_t merge_saved_operations{};
    std::uint64_t requests_deduplicated{};
    std::uint64_t physical_reads_avoided_by_dedup{};
    std::uint64_t bytes_avoided_by_dedup{};
    double read_amplification{};
    std::uint64_t merge_candidates{};
    std::uint64_t merges_accepted{};
    std::uint64_t merges_rejected{};
    std::uint64_t merges_rejected_amplification{};
    std::uint64_t merges_rejected_size{};
    std::uint64_t merges_rejected_gap{};
    std::uint64_t merges_rejected_policy{};
    std::uint64_t intent_sequential_groups{};
    std::uint64_t intent_random_groups{};
    std::uint64_t latency_sensitive_groups{};
    std::uint64_t throughput_groups{};
};

} // namespace intentio
