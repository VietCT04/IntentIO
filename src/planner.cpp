#include "detail/planner.hpp"

#include <algorithm>
#include <limits>
#include <numeric>

namespace intentio::detail {

namespace {

bool add_overflows(std::uint64_t left, std::size_t right) {
    if (right > std::numeric_limits<std::uint64_t>::max()) {
        return true;
    }
    return left > std::numeric_limits<std::uint64_t>::max() -
                    static_cast<std::uint64_t>(right);
}

bool merged_length_fits(const PhysicalRequest& current,
                        const ReadRequest& next,
                        std::size_t max_request_bytes,
                        std::size_t& merged_length) {
    if (next.offset < current.offset) {
        return false;
    }

    const std::uint64_t relative_offset = next.offset - current.offset;
    if (relative_offset > std::numeric_limits<std::size_t>::max() ||
        next.length > std::numeric_limits<std::size_t>::max() -
                           static_cast<std::size_t>(relative_offset)) {
        return false;
    }

    merged_length = static_cast<std::size_t>(relative_offset) + next.length;
    return max_request_bytes == 0 || merged_length <= max_request_bytes;
}

} // namespace

PhysicalPlan plan_reads(const std::vector<ReadRequest>& requests,
                        const GroupHints& hints,
                        const PlannerOptions& options) {
    PhysicalPlan plan;
    if (requests.empty()) {
        return plan;
    }

    std::vector<std::size_t> order(requests.size());
    std::iota(order.begin(), order.end(), 0U);
    std::stable_sort(order.begin(), order.end(), [&requests](std::size_t lhs,
                                                              std::size_t rhs) {
        if (requests[lhs].fd != requests[rhs].fd) {
            return requests[lhs].fd < requests[rhs].fd;
        }
        if (requests[lhs].offset != requests[rhs].offset) {
            return requests[lhs].offset < requests[rhs].offset;
        }
        return lhs < rhs;
    });

    const std::uint64_t max_gap =
        hints.access_pattern == AccessPattern::Random ? 0 : options.max_gap_bytes;

    for (const std::size_t logical_index : order) {
        const ReadRequest& logical = requests[logical_index];
        if (logical.length == 0) {
            continue;
        }

        if (!options.enabled) {
            plan.requests.push_back(PhysicalRequest{
                logical.fd,
                logical.offset,
                logical.length,
                {{logical_index, 0, logical.length}},
            });
            continue;
        }

        bool merged = false;
        if (!plan.requests.empty()) {
            PhysicalRequest& current = plan.requests.back();
            const bool same_fd = current.fd == logical.fd;
            const bool current_end_fits =
                !add_overflows(current.offset, current.length);
            if (same_fd && current_end_fits) {
                const auto current_end =
                    current.offset + static_cast<std::uint64_t>(current.length);
                const bool ordered = logical.offset >= current_end;
                const std::uint64_t gap = ordered ? logical.offset - current_end : 0;
                std::size_t merged_length = 0;
                merged = ordered && gap <= max_gap &&
                         merged_length_fits(current, logical,
                                            options.max_request_bytes,
                                            merged_length);
                if (merged) {
                    current.length = merged_length;
                    current.slices.push_back(PhysicalSlice{
                        logical_index,
                        static_cast<std::size_t>(logical.offset - current.offset),
                        logical.length,
                    });
                }
            }
        }

        if (!merged) {
            plan.requests.push_back(PhysicalRequest{
                logical.fd,
                logical.offset,
                logical.length,
                {{logical_index, 0, logical.length}},
            });
        }
    }

    return plan;
}

} // namespace intentio::detail
