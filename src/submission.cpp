#include "intentio/submission.hpp"

#include "detail/submission_state.hpp"

#include <stdexcept>

namespace intentio::detail {

void SubmissionState::mark_complete() {
    {
        std::lock_guard lock(completion_mutex);
        complete = true;
    }
    completion_cv.notify_all();
}

void SubmissionState::wait() const {
    std::unique_lock lock(completion_mutex);
    completion_cv.wait(lock, [this] { return complete; });
}

bool SubmissionState::ready() const noexcept {
    std::lock_guard lock(completion_mutex);
    return complete;
}

std::vector<ReadResult> SubmissionState::copy_results() const {
    wait();
    std::lock_guard lock(completion_mutex);
    return results;
}

} // namespace intentio::detail

namespace intentio {

void Submission::wait() const {
    if (!state_) {
        throw std::logic_error("cannot wait on an empty submission");
    }
    state_->wait();
}

bool Submission::ready() const noexcept {
    return state_ != nullptr && state_->ready();
}

std::vector<ReadResult> Submission::get() const {
    if (!state_) {
        throw std::logic_error("cannot get an empty submission");
    }
    return state_->copy_results();
}

} // namespace intentio
