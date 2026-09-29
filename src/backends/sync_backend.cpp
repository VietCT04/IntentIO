#include "detail/backends.hpp"

#include "detail/aligned_buffer.hpp"
#include "detail/file_state.hpp"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <chrono>
#include <cstdint>
#include <limits>
#include <fcntl.h>
#include <sys/uio.h>
#include <unistd.h>

namespace intentio::detail {

bool unsupported_dontcache_errno(int error) noexcept {
    return error == EOPNOTSUPP || error == ENOTSUP;
}

void record_advice(PhysicalRead& read,
                   const KernelIOPolicy& policy) {
    if (policy.access_advice == intentio::FileAdvice::None ||
        read.request.offset >
            static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()) ||
        read.request.length >
            static_cast<std::size_t>(std::numeric_limits<off_t>::max()) -
                static_cast<std::size_t>(read.request.offset)) {
        return;
    }

    int advice = POSIX_FADV_NORMAL;
    switch (policy.access_advice) {
    case intentio::FileAdvice::None:
        return;
    case intentio::FileAdvice::Sequential:
        advice = POSIX_FADV_SEQUENTIAL;
        break;
    case intentio::FileAdvice::Random:
        advice = POSIX_FADV_RANDOM;
        break;
    case intentio::FileAdvice::NoReuse:
        advice = POSIX_FADV_NOREUSE;
        break;
    }

    const int result = ::posix_fadvise(
        read.request.fd,
        static_cast<off_t>(read.request.offset),
        static_cast<off_t>(read.request.length),
        advice);
    if (result != 0) {
        ++read.kernel_io_stats.kernel_advice_failures;
        return;
    }
    switch (policy.access_advice) {
    case intentio::FileAdvice::None:
        break;
    case intentio::FileAdvice::Sequential:
        ++read.kernel_io_stats.kernel_advice_sequential;
        break;
    case intentio::FileAdvice::Random:
        ++read.kernel_io_stats.kernel_advice_random;
        break;
    case intentio::FileAdvice::NoReuse:
        ++read.kernel_io_stats.kernel_advice_noreuse;
        break;
    }
}

void execute_buffered(PhysicalRead& read) {
    ++read.kernel_io_stats.buffered_reads;
    read.bytes_read = 0;
    std::size_t remaining = read.request.length;
    std::size_t completed = 0;
    while (remaining != 0) {
        const std::size_t count =
            remaining > static_cast<std::size_t>(SSIZE_MAX)
                ? static_cast<std::size_t>(SSIZE_MAX)
                : remaining;
        const auto offset = static_cast<off_t>(read.request.offset + completed);
        const ssize_t result = ::pread(read.request.fd,
                                       read.data.data() + completed,
                                       count,
                                       offset);
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            read.error = std::error_code(errno, std::generic_category());
            break;
        }
        if (result == 0) {
            break;
        }

        completed += static_cast<std::size_t>(result);
        remaining -= static_cast<std::size_t>(result);
        if (static_cast<std::size_t>(result) < count) {
            break;
        }
    }
    read.bytes_read = completed;
    read.kernel_io_stats.bytes_buffered += completed;
}

bool is_aligned(std::uint64_t value, std::size_t alignment) noexcept {
    return alignment != 0 && value % alignment == 0;
}

bool is_size_aligned(std::size_t value, std::size_t alignment) noexcept {
    return alignment != 0 && value % alignment == 0;
}

bool execute_direct(PhysicalRead& read, bool allow_fallback) {
    ++read.kernel_io_stats.direct_reads_attempted;
    if (!read.request.managed_file) {
        ++read.kernel_io_stats.direct_unsupported;
        if (allow_fallback) {
            return false;
        }
        read.error = std::make_error_code(std::errc::operation_not_supported);
        return true;
    }

    const auto& file = *read.request.managed_file;
    const auto& capabilities = file.capabilities;
    if (!capabilities.direct_io_supported || file.direct_fd < 0) {
        ++read.kernel_io_stats.direct_unsupported;
        if (file.direct_open_errno != 0) {
            ++read.kernel_io_stats.direct_open_failures;
        }
        if (allow_fallback) {
            return false;
        }
        read.error = std::make_error_code(std::errc::operation_not_supported);
        return true;
    }
    if (!is_aligned(read.request.offset, capabilities.read_offset_alignment) ||
        !is_size_aligned(read.request.length, capabilities.offset_alignment)) {
        ++read.kernel_io_stats.direct_alignment_fallbacks;
        if (allow_fallback) {
            return false;
        }
        read.error = std::make_error_code(std::errc::invalid_argument);
        return true;
    }

    std::error_code allocation_error;
    AlignedBuffer buffer = AlignedBuffer::allocate(
        read.request.length,
        capabilities.memory_alignment,
        allocation_error);
    if (buffer.data() == nullptr) {
        read.error = allocation_error.value() == 0
                         ? std::make_error_code(std::errc::not_enough_memory)
                         : allocation_error;
        return true;
    }
    if (reinterpret_cast<std::uintptr_t>(buffer.data()) %
            capabilities.memory_alignment !=
        0U) {
        ++read.kernel_io_stats.direct_alignment_fallbacks;
        if (allow_fallback) {
            return false;
        }
        read.error = std::make_error_code(std::errc::invalid_argument);
        return true;
    }

    read.bytes_read = 0;
    std::size_t remaining = read.request.length;
    std::size_t completed = 0;
    while (remaining != 0) {
        const std::size_t count =
            remaining > static_cast<std::size_t>(SSIZE_MAX)
                ? static_cast<std::size_t>(SSIZE_MAX)
                : remaining;
        const ssize_t result = ::pread(
            file.direct_fd,
            buffer.data() + completed,
            count,
            static_cast<off_t>(read.request.offset + completed));
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            read.error = std::error_code(errno, std::generic_category());
            break;
        }
        if (result == 0) {
            break;
        }
        completed += static_cast<std::size_t>(result);
        remaining -= static_cast<std::size_t>(result);
        if (static_cast<std::size_t>(result) < count) {
            break;
        }
    }

    read.bytes_read = completed;
    if (!read.error) {
        std::copy_n(buffer.data(), completed, read.data.data());
        ++read.kernel_io_stats.direct_reads_succeeded;
        read.kernel_io_stats.direct_bytes += completed;
    }
    return true;
}

bool execute_dontcache(PhysicalRead& read) {
    ++read.kernel_io_stats.dontcache_reads_attempted;
    read.bytes_read = 0;
    std::size_t remaining = read.request.length;
    std::size_t completed = 0;
    bool successful = true;
    while (remaining != 0) {
        const std::size_t count =
            remaining > static_cast<std::size_t>(SSIZE_MAX)
                ? static_cast<std::size_t>(SSIZE_MAX)
                : remaining;
        iovec vector{
            read.data.data() + completed,
            count,
        };
        const auto offset = static_cast<off_t>(read.request.offset + completed);
        const ssize_t result = ::preadv2(read.request.fd,
                                          &vector,
                                          1,
                                          offset,
                                          RWF_DONTCACHE);
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (completed == 0 && unsupported_dontcache_errno(errno)) {
                return false;
            }
            read.error = std::error_code(errno, std::generic_category());
            successful = false;
            break;
        }
        if (result == 0) {
            break;
        }

        completed += static_cast<std::size_t>(result);
        remaining -= static_cast<std::size_t>(result);
        if (static_cast<std::size_t>(result) < count) {
            break;
        }
    }
    read.bytes_read = completed;
    if (successful) {
        ++read.kernel_io_stats.dontcache_reads_succeeded;
    }
    read.kernel_io_stats.bytes_dontcache += completed;
    return true;
}

void execute_pread(PhysicalRead& read, const KernelIOPolicy& policy) {
    read.bytes_read = 0;
    read.error = {};
    read.kernel_io_stats = {};
    if (read.request.length == 0) {
        return;
    }

    if (read.request.managed_file) {
        ++read.kernel_io_stats.managed_file_reads;
    }

    if (read.request.offset >
            static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()) ||
        read.request.length >
            static_cast<std::size_t>(std::numeric_limits<off_t>::max()) -
                static_cast<std::size_t>(read.request.offset)) {
        read.error = std::make_error_code(std::errc::value_too_large);
        return;
    }

    intentio::CachePolicy selected_cache_policy = policy.cache_policy;
    if (policy.requested_mode == intentio::IOCacheMode::Auto &&
        policy.auto_direct_threshold_bytes != 0 &&
        read.request.managed_file &&
        read.request.length >= policy.auto_direct_threshold_bytes) {
        selected_cache_policy = intentio::CachePolicy::Direct;
    }

    if (policy.requested_mode == intentio::IOCacheMode::Auto) {
        switch (selected_cache_policy) {
        case intentio::CachePolicy::Normal:
            ++read.kernel_io_stats.auto_selected_buffered;
            break;
        case intentio::CachePolicy::DontCache:
            ++read.kernel_io_stats.auto_selected_dontcache;
            break;
        case intentio::CachePolicy::Direct:
            ++read.kernel_io_stats.auto_selected_direct;
            break;
        }
    }

    record_advice(read, policy);
    if (selected_cache_policy == intentio::CachePolicy::Direct) {
        if (execute_direct(read,
                           policy.requested_mode == intentio::IOCacheMode::Auto)) {
            return;
        }
        selected_cache_policy = policy.cache_policy;
    }
    if (selected_cache_policy == intentio::CachePolicy::DontCache &&
        execute_dontcache(read)) {
        return;
    }
    if (selected_cache_policy == intentio::CachePolicy::DontCache) {
        ++read.kernel_io_stats.dontcache_unsupported;
        ++read.kernel_io_stats.dontcache_fallbacks;
        read.error = {};
        read.bytes_read = 0;
    }
    execute_buffered(read);
}

ExecutionStats SyncBackend::execute(
    std::vector<PhysicalRead>& reads,
    const intentio::ExecutionPolicy& policy,
    const KernelIOPolicy& kernel_policy) const {
    (void)policy;
    ExecutionStats stats;
    const auto start = std::chrono::steady_clock::now();
    for (PhysicalRead& read : reads) {
        const auto operation_start = std::chrono::steady_clock::now();
        execute_pread(read, kernel_policy);
        const auto operation_stop = std::chrono::steady_clock::now();
        stats.total_latency_ns += static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                operation_stop - operation_start)
                .count());
        add_kernel_io_stats(stats.kernel_io, read.kernel_io_stats);
    }
    const auto stop = std::chrono::steady_clock::now();
    stats.completed = reads.size();
    stats.peak_in_flight = reads.empty() ? 0 : 1;
    stats.elapsed_ns = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(stop - start)
            .count());
    return stats;
}

} // namespace intentio::detail
