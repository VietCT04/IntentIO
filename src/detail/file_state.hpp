#pragma once

#include "intentio/file.hpp"

#include <string>

namespace intentio::detail {

struct FileState {
    FileState() = default;

    int buffered_fd{-1};
    int direct_fd{-1};
    int direct_open_errno{};
    std::string path;
    FileCapabilities capabilities{};

    ~FileState();

    FileState(const FileState&) = delete;
    FileState& operator=(const FileState&) = delete;
};

} // namespace intentio::detail
