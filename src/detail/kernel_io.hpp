#pragma once

#include "intentio/hints.hpp"
#include "intentio/kernel_io.hpp"

#include <cstddef>
#include <cstdint>

namespace intentio::detail {

struct KernelIOPolicy {
    intentio::FileAdvice access_advice{intentio::FileAdvice::None};
    intentio::CachePolicy cache_policy{intentio::CachePolicy::Normal};
};

struct KernelIOStats {
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
};

[[nodiscard]] KernelIOPolicy resolve_kernel_io_policy(
    const intentio::KernelIOOptions& options,
    const intentio::GroupHints& hints);

[[nodiscard]] const char* file_advice_name(intentio::FileAdvice advice) noexcept;
[[nodiscard]] const char* cache_policy_name(intentio::CachePolicy policy) noexcept;

void add_kernel_io_stats(KernelIOStats& destination,
                         const KernelIOStats& source) noexcept;

} // namespace intentio::detail
