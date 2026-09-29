#pragma once

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

enum class Priority {
    Low,
    Normal,
    High,
};

// ReuseHint and Priority are defined now so the public vocabulary does not
// need to change when their planner/executor policies arrive. The initial
// release intentionally only applies AccessPattern.
struct GroupHints {
    AccessPattern access_pattern{AccessPattern::Unknown};
    ReuseHint reuse_hint{ReuseHint::Unknown};
    Priority priority{Priority::Normal};
};

} // namespace intentio
