#pragma once

#include "hints.hpp"
#include "file.hpp"
#include "request.hpp"
#include "result.hpp"

#include <cstddef>
#include <memory>
#include <vector>

namespace intentio {

namespace detail {
struct RuntimeState;
}

class Group {
public:
    Group(Group&&) noexcept;
    Group& operator=(Group&&) noexcept;
    Group(const Group&) = delete;
    Group& operator=(const Group&) = delete;
    ~Group();

    RequestId read(int fd, std::uint64_t offset, std::size_t length);
    RequestId read(const File& file,
                   std::uint64_t offset,
                   std::size_t length);

    void hint(AccessPattern pattern) noexcept;
    void access_pattern(AccessPattern pattern) noexcept { hint(pattern); }
    void hint(ReuseHint hint) noexcept;
    void reuse_hint(ReuseHint hint) noexcept { this->hint(hint); }
    void latency_class(LatencyClass latency) noexcept;
    void io_cache_mode(IOCacheMode mode) noexcept;

    [[nodiscard]] const GroupHints& hints() const noexcept { return hints_; }
    [[nodiscard]] std::size_t size() const noexcept { return requests_.size(); }

    // A group is single-use. Results are ordered in the same order as read().
    [[nodiscard]] std::vector<ReadResult> submit();

private:
    friend class Runtime;
    explicit Group(std::shared_ptr<detail::RuntimeState> state);

    std::shared_ptr<detail::RuntimeState> state_;
    std::vector<ReadRequest> requests_;
    GroupHints hints_{};
    RequestId next_request_id_{1};
    bool submitted_{false};
};

} // namespace intentio
