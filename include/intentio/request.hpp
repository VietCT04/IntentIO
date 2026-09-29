#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

namespace intentio::detail {
struct FileState;
}

namespace intentio {

using RequestId = std::uint64_t;

struct ReadRequest {
    RequestId id{};
    int fd{-1};
    std::uint64_t offset{};
    std::size_t length{};
    std::shared_ptr<detail::FileState> managed_file{};
};

} // namespace intentio
