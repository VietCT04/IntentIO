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

class SyncBackend {
public:
    void execute(std::vector<PhysicalRead>& reads) const;
};

class IoUringBackend {
public:
    struct Impl;

    explicit IoUringBackend(std::size_t queue_depth);
    IoUringBackend(const IoUringBackend&) = delete;
    IoUringBackend& operator=(const IoUringBackend&) = delete;
    ~IoUringBackend();

    void execute(std::vector<PhysicalRead>& reads);

private:
    Impl* impl_{};
};

} // namespace intentio::detail
