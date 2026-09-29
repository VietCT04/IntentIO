#pragma once

#include "intentio/hints.hpp"
#include "intentio/runtime.hpp"

namespace intentio::detail {

intentio::PlanningPolicy resolve_policy(const intentio::RuntimeOptions& options,
                                         const intentio::GroupHints& hints);

} // namespace intentio::detail
