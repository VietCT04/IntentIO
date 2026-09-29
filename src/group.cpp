#include "intentio/group.hpp"

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

void Group::hint(AccessPattern pattern) noexcept {
    hints_.access_pattern = pattern;
}

void Group::hint(ReuseHint hint) noexcept {
    hints_.reuse_hint = hint;
}

void Group::latency_class(LatencyClass latency) noexcept {
    hints_.latency_class = latency;
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
