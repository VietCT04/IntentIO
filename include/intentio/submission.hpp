#pragma once

#include "result.hpp"

#include <memory>
#include <vector>

namespace intentio {

namespace detail {
struct SubmissionState;
struct RuntimeState;
}

class Submission {
public:
    Submission() noexcept = default;
    Submission(Submission&&) noexcept = default;
    Submission& operator=(Submission&&) noexcept = default;
    Submission(const Submission&) = delete;
    Submission& operator=(const Submission&) = delete;
    ~Submission() = default;

    void wait() const;
    [[nodiscard]] bool ready() const noexcept;
    [[nodiscard]] std::vector<ReadResult> get() const;

private:
    friend class Runtime;
    friend struct detail::RuntimeState;
    Submission(std::shared_ptr<detail::SubmissionState> state,
               std::shared_ptr<void> keep_alive)
        : state_(std::move(state)), keep_alive_(std::move(keep_alive)) {}

    std::shared_ptr<detail::SubmissionState> state_;
    std::shared_ptr<void> keep_alive_;
};

} // namespace intentio
