#pragma once

#include <cstddef>
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
    std::uint64_t concurrency_increases{};
    std::uint64_t concurrency_decreases{};
    std::size_t current_concurrency{1};
    std::size_t peak_concurrency{};
    std::uint64_t controller_windows{};
    double ewma_completion_latency_ms{};
    double completion_rate{};
    std::uint64_t executor_tasks_submitted{};
    std::uint64_t executor_tasks_completed{};
    std::uint64_t executor_queue_depth{};
    std::uint64_t executor_queue_wait_ns{};
    std::uint64_t executor_execution_ns{};
    std::uint64_t executor_completion_dispatch_ns{};
    std::uint64_t executor_worker_wakeups{};
    std::uint64_t executor_spurious_wakeups{};
    std::uint64_t executor_workers_busy{};
    std::uint64_t executor_workers_idle{};
    std::uint64_t executor_max_queue_depth{};
    std::uint64_t executor_peak_in_flight{};
    std::uint64_t kernel_advice_sequential{};
    std::uint64_t kernel_advice_random{};
    std::uint64_t kernel_advice_noreuse{};
    std::uint64_t kernel_advice_failures{};
    std::uint64_t dontcache_reads_attempted{};
    std::uint64_t dontcache_reads_succeeded{};
    std::uint64_t dontcache_unsupported{};
    std::uint64_t dontcache_fallbacks{};
    std::uint64_t buffered_reads{};
    std::uint64_t bytes_dontcache{};
    std::uint64_t bytes_buffered{};
    std::uint64_t managed_file_reads{};
    std::uint64_t direct_reads_attempted{};
    std::uint64_t direct_reads_succeeded{};
    std::uint64_t direct_bytes{};
    std::uint64_t direct_unsupported{};
    std::uint64_t direct_alignment_fallbacks{};
    std::uint64_t direct_open_failures{};
    std::uint64_t auto_selected_buffered{};
    std::uint64_t auto_selected_dontcache{};
    std::uint64_t auto_selected_direct{};
};

} // namespace intentio
