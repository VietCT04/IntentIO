#include "detail/kernel_io.hpp"

namespace intentio::detail {

KernelIOPolicy resolve_kernel_io_policy(
    const intentio::KernelIOOptions& options,
    const intentio::GroupHints& hints) {
    KernelIOPolicy policy;
    if (!options.enabled) {
        return policy;
    }

    if (options.enable_fadvise) {
        switch (hints.access_pattern) {
        case intentio::AccessPattern::Unknown:
            break;
        case intentio::AccessPattern::Sequential:
            policy.access_advice = intentio::FileAdvice::Sequential;
            break;
        case intentio::AccessPattern::Random:
            policy.access_advice = intentio::FileAdvice::Random;
            break;
        }
    }

    if (hints.reuse_hint == intentio::ReuseHint::ScanOnce) {
        if (options.enable_dontcache) {
            policy.cache_policy = intentio::CachePolicy::DontCache;
        } else if (options.enable_fadvise &&
                   policy.access_advice == intentio::FileAdvice::None) {
            policy.access_advice = intentio::FileAdvice::NoReuse;
        }
    }

    return policy;
}

const char* file_advice_name(intentio::FileAdvice advice) noexcept {
    switch (advice) {
    case intentio::FileAdvice::None:
        return "NONE";
    case intentio::FileAdvice::Sequential:
        return "SEQUENTIAL";
    case intentio::FileAdvice::Random:
        return "RANDOM";
    case intentio::FileAdvice::NoReuse:
        return "NOREUSE";
    }
    return "NONE";
}

const char* cache_policy_name(intentio::CachePolicy policy) noexcept {
    switch (policy) {
    case intentio::CachePolicy::Normal:
        return "NORMAL";
    case intentio::CachePolicy::DontCache:
        return "DONTCACHE";
    }
    return "NORMAL";
}

void add_kernel_io_stats(KernelIOStats& destination,
                         const KernelIOStats& source) noexcept {
    destination.kernel_advice_sequential += source.kernel_advice_sequential;
    destination.kernel_advice_random += source.kernel_advice_random;
    destination.kernel_advice_noreuse += source.kernel_advice_noreuse;
    destination.kernel_advice_failures += source.kernel_advice_failures;
    destination.dontcache_reads_attempted += source.dontcache_reads_attempted;
    destination.dontcache_reads_succeeded += source.dontcache_reads_succeeded;
    destination.dontcache_unsupported += source.dontcache_unsupported;
    destination.dontcache_fallbacks += source.dontcache_fallbacks;
    destination.buffered_reads += source.buffered_reads;
    destination.bytes_dontcache += source.bytes_dontcache;
    destination.bytes_buffered += source.bytes_buffered;
}

} // namespace intentio::detail
