#include "detail/backends.hpp"

#include <cerrno>
#include <climits>
#include <chrono>
#include <cstdint>
#include <limits>
#include <unistd.h>

namespace intentio::detail {

void execute_pread(PhysicalRead& read) {
    read.bytes_read = 0;
    read.error = {};
    if (read.request.length == 0) {
        return;
    }

    if (read.request.offset >
            static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()) ||
        read.request.length >
            static_cast<std::size_t>(std::numeric_limits<off_t>::max()) -
                static_cast<std::size_t>(read.request.offset)) {
        read.error = std::make_error_code(std::errc::value_too_large);
        return;
    }

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
}

ExecutionStats SyncBackend::execute(
    std::vector<PhysicalRead>& reads,
    const intentio::ExecutionPolicy& policy) const {
    (void)policy;
    ExecutionStats stats;
    const auto start = std::chrono::steady_clock::now();
    for (PhysicalRead& read : reads) {
        const auto operation_start = std::chrono::steady_clock::now();
        execute_pread(read);
        const auto operation_stop = std::chrono::steady_clock::now();
        stats.total_latency_ns += static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                operation_stop - operation_start)
                .count());
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
