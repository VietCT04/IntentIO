#pragma once

#include <cstdint>

namespace intentio {

struct MetricsSnapshot {
    std::uint64_t logical_reads{};
    std::uint64_t physical_reads{};
    std::uint64_t logical_bytes{};
    std::uint64_t physical_bytes{};
    std::uint64_t requests_merged{};
    std::uint64_t merge_saved_operations{};
};

} // namespace intentio
