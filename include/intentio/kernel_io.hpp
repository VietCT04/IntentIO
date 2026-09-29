#pragma once

#include <cstddef>

namespace intentio {

enum class IOCacheMode {
    Auto,
    Buffered,
    DontCache,
    Direct,
};

enum class FileAdvice {
    None,
    Sequential,
    Random,
    NoReuse,
};

enum class CachePolicy {
    Normal,
    DontCache,
    Direct,
};

struct KernelIOOptions {
    bool enabled{true};
    bool enable_fadvise{true};
    bool enable_dontcache{true};
    // Experimental only. Zero keeps automatic direct-I/O selection disabled.
    std::size_t auto_direct_threshold_bytes{};
};

} // namespace intentio
