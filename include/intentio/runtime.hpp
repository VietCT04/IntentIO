#pragma once

#include "group.hpp"
#include "metrics.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>

namespace intentio {

enum class Backend {
    Synchronous,
    IoUring,
    Auto,
};

struct CoalescingOptions {
    bool enabled{true};
    std::uint64_t max_gap_bytes{0};
    std::size_t max_request_bytes{8U * 1024U * 1024U};
};

struct RuntimeOptions {
    Backend backend{Backend::Synchronous};
    std::size_t queue_depth{32};
    CoalescingOptions coalescing{};
};

class Runtime {
public:
    explicit Runtime(RuntimeOptions options = {});
    Runtime(Runtime&&) noexcept;
    Runtime& operator=(Runtime&&) noexcept;
    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;
    ~Runtime();

    [[nodiscard]] Group create_group();
    [[nodiscard]] Backend backend() const noexcept;
    [[nodiscard]] MetricsSnapshot metrics() const;

private:
    std::shared_ptr<detail::RuntimeState> state_;
};

} // namespace intentio
