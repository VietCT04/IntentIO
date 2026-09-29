#pragma once

#include "intentio/execution.hpp"
#include "intentio/hints.hpp"
#include "intentio/runtime.hpp"

namespace intentio::detail {

struct ResolvedExecutionPolicy {
    intentio::ExecutionPolicy fixed{};
    intentio::ConcurrencyPolicy adaptive{};
    bool adaptive_enabled{false};
};

ResolvedExecutionPolicy resolve_execution_policy(
    const intentio::RuntimeOptions& options,
    const intentio::GroupHints& hints);

} // namespace intentio::detail
