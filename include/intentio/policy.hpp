#pragma once

#include <cstddef>
#include <cstdint>

namespace intentio {

struct PlanningPolicy {
    std::size_t max_merge_gap_bytes{4U * 1024U};
    std::size_t max_merged_size_bytes{8U * 1024U * 1024U};
    double max_read_amplification{1.5};
    bool enable_coalescing{true};
    bool enable_deduplication{true};
};

struct IntentPolicyOptions {
    PlanningPolicy unknown{};
    PlanningPolicy sequential{
        64U * 1024U,
        8U * 1024U * 1024U,
        1.75,
        true,
        true,
    };
    PlanningPolicy random{
        0,
        1U * 1024U * 1024U,
        1.10,
        true,
        true,
    };
    PlanningPolicy latency_sensitive{
        0,
        256U * 1024U,
        1.10,
        true,
        true,
    };
    PlanningPolicy throughput{
        32U * 1024U,
        16U * 1024U * 1024U,
        2.00,
        true,
        true,
    };
};

} // namespace intentio
