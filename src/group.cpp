#include "intentio/group.hpp"

#include "detail/file_state.hpp"
#include "detail/runtime_state.hpp"

#include <stdexcept>

namespace intentio {

Group::Group(std::shared_ptr<detail::RuntimeState> state)
    : state_(std::move(state)) {}

Group::Group(Group&&) noexcept = default;

Group& Group::operator=(Group&&) noexcept = default;

Group::~Group() = default;

RequestId Group::read(int fd, std::uint64_t offset, std::size_t length) {
    if (submitted_) {
        throw std::logic_error("cannot add a read after Group::submit");
    }
    if (next_request_id_ == 0) {
        throw std::overflow_error("IntentIO request id exhausted");
    }

    const RequestId id = next_request_id_++;
    requests_.push_back(ReadRequest{id, fd, offset, length});
    return id;
}

RequestId Group::read(const File& file,
                      std::uint64_t offset,
                      std::size_t length) {
    if (submitted_) {
        throw std::logic_error("cannot add a read after Group::submit");
    }
    if (next_request_id_ == 0) {
        throw std::overflow_error("IntentIO request id exhausted");
    }
    if (!file.state_ || file.state_->buffered_fd < 0) {
        throw std::invalid_argument("cannot read from an invalid IntentIO file");
    }

    const RequestId id = next_request_id_++;
    requests_.push_back(ReadRequest{
        id,
        file.state_->buffered_fd,
        offset,
        length,
        file.state_,
    });
    return id;
}

void Group::hint(AccessPattern pattern) noexcept {
    hints_.access_pattern = pattern;
}

void Group::hint(ReuseHint hint) noexcept {
    hints_.reuse_hint = hint;
}

void Group::latency_class(LatencyClass latency) noexcept {
    hints_.latency_class = latency;
}

void Group::io_cache_mode(IOCacheMode mode) noexcept {
    hints_.io_cache_mode = mode;
}

std::vector<ReadResult> Group::submit() {
    if (submitted_) {
        throw std::logic_error("Group::submit may only be called once");
    }
    if (!state_) {
        throw std::logic_error("cannot submit a moved-from group");
    }

    submitted_ = true;
    return state_->submit(requests_, hints_);
}

} // namespace intentio
