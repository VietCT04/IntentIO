#include "detail/backends.hpp"

#include <cerrno>
#include <climits>
#include <cstdint>
#include <limits>
#include <unistd.h>

namespace intentio::detail {

void SyncBackend::execute(std::vector<PhysicalRead>& reads) const {
    for (PhysicalRead& read : reads) {
        if (read.request.length == 0) {
            continue;
        }

        if (read.request.offset >
                static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()) ||
            read.request.length >
                static_cast<std::size_t>(std::numeric_limits<off_t>::max()) -
                    static_cast<std::size_t>(read.request.offset)) {
            read.error = std::make_error_code(std::errc::value_too_large);
            continue;
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
}

} // namespace intentio::detail
