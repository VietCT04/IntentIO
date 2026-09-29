#pragma once

#include "request.hpp"

#include <cstddef>
#include <span>
#include <system_error>
#include <vector>

namespace intentio {

struct ReadResult {
    RequestId request_id{};
    std::uint64_t offset{};
    std::size_t requested_bytes{};
    std::vector<std::byte> data{};
    std::error_code error{};

    [[nodiscard]] bool ok() const noexcept { return !error; }
    [[nodiscard]] std::size_t bytes_read() const noexcept { return data.size(); }
    [[nodiscard]] std::span<const std::byte> bytes() const noexcept { return data; }
};

} // namespace intentio
