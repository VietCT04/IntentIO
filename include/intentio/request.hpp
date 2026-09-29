#pragma once

#include <cstddef>
#include <cstdint>

namespace intentio {

using RequestId = std::uint64_t;

struct ReadRequest {
    RequestId id{};
    int fd{-1};
    std::uint64_t offset{};
    std::size_t length{};
};

} // namespace intentio
