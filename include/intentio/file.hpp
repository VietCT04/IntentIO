#pragma once

#include <cstddef>
#include <memory>
#include <string>

namespace intentio {

namespace detail {
struct FileState;
}

struct FileCapabilities {
    bool direct_io_supported{false};
    std::size_t memory_alignment{};
    std::size_t offset_alignment{};
    std::size_t read_offset_alignment{};
    bool dontcache_supported{false};
};

class File {
public:
    static File open(const std::string& path);

    File(File&&) noexcept;
    File& operator=(File&&) noexcept;
    File(const File&) = delete;
    File& operator=(const File&) = delete;
    ~File();

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] FileCapabilities capabilities() const noexcept;

private:
    friend class Group;

    explicit File(std::shared_ptr<detail::FileState> state);

    std::shared_ptr<detail::FileState> state_;
};

} // namespace intentio
