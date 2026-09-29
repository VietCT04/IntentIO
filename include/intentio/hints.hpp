#pragma once

#include "kernel_io.hpp"

#include <cstddef>
#include <cstdint>

namespace intentio {

enum class AccessPattern {
    Unknown,
    Sequential,
    Random,
};

enum class ReuseHint {
    Unknown,
    ReuseLikely,
    ScanOnce,
};

// Kept for source compatibility with the original public vocabulary. Phase 5
// intentionally resolves only the access, reuse, and latency hints above.
enum class Priority {
    Low,
    Normal,
    High,
};

enum class LatencyClass {
    Default,
    LatencySensitive,
    Throughput,
};

struct GroupHints {
    AccessPattern access_pattern{AccessPattern::Unknown};
    ReuseHint reuse_hint{ReuseHint::Unknown};
    Priority priority{Priority::Normal};
    LatencyClass latency_class{LatencyClass::Default};
    IOCacheMode io_cache_mode{IOCacheMode::Auto};
};

} // namespace intentio
