#pragma once

namespace intentio {

enum class FileAdvice {
    None,
    Sequential,
    Random,
    NoReuse,
};

enum class CachePolicy {
    Normal,
    DontCache,
};

struct KernelIOOptions {
    bool enabled{true};
    bool enable_fadvise{true};
    bool enable_dontcache{true};
};

} // namespace intentio
