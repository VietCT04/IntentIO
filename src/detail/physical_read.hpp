#pragma once

#include "planner.hpp"

#include <cstddef>
#include <system_error>
#include <vector>

namespace intentio::detail {

struct PhysicalRead {
    PhysicalRequest request;
    std::vector<std::byte> data;
    std::size_t bytes_read{};
    std::error_code error{};
};

} // namespace intentio::detail
