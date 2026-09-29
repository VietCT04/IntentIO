#include "detail/planner.hpp"

#include <algorithm>
#include <limits>
#include <numeric>
#include <sstream>

namespace intentio::detail {

namespace {

bool end_offset(std::uint64_t offset,
                std::size_t length,
                std::uint64_t& end) {
    if (length > std::numeric_limits<std::uint64_t>::max() ||
        offset > std::numeric_limits<std::uint64_t>::max() -
                     static_cast<std::uint64_t>(length)) {
        return false;
    }
    end = offset + static_cast<std::uint64_t>(length);
    return true;
}

void add_saturating(std::uint64_t value, std::uint64_t& total) {
    if (value > std::numeric_limits<std::uint64_t>::max() - total) {
        total = std::numeric_limits<std::uint64_t>::max();
    } else {
        total += value;
    }
}

std::vector<std::size_t> sorted_order(
    const std::vector<ReadRequest>& requests) {
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
        if (requests[lhs].length != requests[rhs].length) {
            return requests[lhs].length < requests[rhs].length;
        }
        return lhs < rhs;
    });
    return order;
}

std::uint64_t unique_bytes(const std::vector<ReadRequest>& requests,
                           const std::vector<std::size_t>& order) {
    std::uint64_t total = 0;
    int current_fd = -1;
    std::uint64_t current_end = 0;
    bool have_range = false;

    for (const std::size_t logical_index : order) {
        const ReadRequest& request = requests[logical_index];
        if (request.length == 0) {
            continue;
        }

        std::uint64_t request_end = 0;
        if (!end_offset(request.offset, request.length, request_end)) {
            add_saturating(static_cast<std::uint64_t>(request.length), total);
            have_range = false;
            continue;
        }

        if (!have_range || request.fd != current_fd || request.offset > current_end) {
            add_saturating(static_cast<std::uint64_t>(request.length), total);
            current_fd = request.fd;
            current_end = request_end;
            have_range = true;
            continue;
        }

        if (request_end > current_end) {
            add_saturating(request_end - current_end, total);
            current_end = request_end;
        }
    }
    return total;
}

bool merged_length_fits(const PhysicalRequest& current,
                        const ReadRequest& next,
                        std::uint64_t union_end,
                        std::size_t max_merged_size_bytes,
                        std::size_t& merged_length) {
    if (next.offset < current.offset) {
        return false;
    }

    if (union_end < current.offset ||
        union_end - current.offset > std::numeric_limits<std::size_t>::max()) {
        return false;
    }

    merged_length = static_cast<std::size_t>(union_end - current.offset);
    return max_merged_size_bytes == 0 ||
           merged_length <= max_merged_size_bytes;
}

PhysicalRequest make_physical_request(const ReadRequest& logical,
                                      std::size_t logical_index) {
    PhysicalRequest physical{
        logical.fd,
        logical.offset,
        logical.length,
        {{logical_index, 0, logical.length}},
        static_cast<std::uint64_t>(logical.length),
        0,
    };
    end_offset(logical.offset, logical.length, physical.coverage_end);
    return physical;
}

void trace_candidate(PhysicalPlan& plan,
                     const ReadRequest& logical,
                     std::uint64_t useful_bytes,
                     std::size_t physical_bytes,
                     double amplification,
                     const char* decision) {
    std::ostringstream stream;
    stream << "candidate logical=" << logical.id
           << " fd=" << logical.fd
           << " useful_bytes=" << useful_bytes
           << " physical_bytes=" << physical_bytes
           << " amplification=" << amplification
           << " " << decision << '\n';
    plan.trace += stream.str();
}

} // namespace

PhysicalPlan plan_reads(const std::vector<ReadRequest>& requests,
                        const PlanningPolicy& policy,
                        const PlannerOptions& options) {
    PhysicalPlan plan;
    if (requests.empty()) {
        return plan;
    }

    const std::vector<std::size_t> order = sorted_order(requests);
    plan.unique_logical_bytes = unique_bytes(requests, order);

    for (const std::size_t logical_index : order) {
        const ReadRequest& logical = requests[logical_index];
        if (logical.length == 0) {
            continue;
        }

        if (!policy.enable_coalescing && !policy.enable_deduplication) {
            plan.requests.push_back(make_physical_request(logical, logical_index));
            continue;
        }

        bool merged = false;
        if (!plan.requests.empty()) {
            PhysicalRequest& current = plan.requests.back();
            const bool same_fd = current.fd == logical.fd;
            std::uint64_t current_end = 0;
            std::uint64_t logical_end = 0;
            const bool current_end_fits =
                end_offset(current.offset, current.length, current_end);
            const bool logical_end_fits =
                end_offset(logical.offset, logical.length, logical_end);
            if (same_fd && current_end_fits && logical_end_fits) {
                const bool overlap = logical.offset < current_end;
                const std::uint64_t gap =
                    logical.offset >= current_end ? logical.offset - current_end : 0;
                const std::uint64_t union_end =
                    std::max(current_end, logical_end);
                std::size_t merged_length = 0;
                ++plan.merge_candidates;

                const bool should_deduplicate = overlap;
                const bool should_coalesce = !overlap;
                const bool policy_allows =
                    should_deduplicate ? policy.enable_deduplication
                                      : policy.enable_coalescing;
                if (!policy_allows) {
                    ++plan.merges_rejected;
                    ++plan.merges_rejected_policy;
                    if (options.trace) {
                        trace_candidate(plan, logical, current.useful_bytes,
                                        current.length, 0.0,
                                        "REJECTED: policy");
                    }
                } else if (should_coalesce && gap > policy.max_merge_gap_bytes) {
                    ++plan.merges_rejected;
                    ++plan.merges_rejected_gap;
                    if (options.trace) {
                        trace_candidate(plan, logical, current.useful_bytes,
                                        current.length, 0.0,
                                        "REJECTED: gap");
                    }
                } else if (!merged_length_fits(current, logical, union_end,
                                               policy.max_merged_size_bytes,
                                               merged_length)) {
                    ++plan.merges_rejected;
                    ++plan.merges_rejected_size;
                    if (options.trace) {
                        trace_candidate(plan, logical, current.useful_bytes,
                                        current.length, 0.0,
                                        "REJECTED: size");
                    }
                } else {
                    const std::uint64_t overlap_end =
                        std::min(current.coverage_end, logical_end);
                    const std::uint64_t covered_overlap =
                        logical.offset < current.coverage_end
                            ? overlap_end - logical.offset
                            : 0;
                    const std::uint64_t candidate_useful =
                        current.useful_bytes +
                        static_cast<std::uint64_t>(logical.length) -
                        covered_overlap;
                    const double amplification =
                        candidate_useful == 0
                            ? std::numeric_limits<double>::infinity()
                            : static_cast<double>(merged_length) /
                                  static_cast<double>(candidate_useful);
                    if (amplification > policy.max_read_amplification) {
                        ++plan.merges_rejected;
                        ++plan.merges_rejected_amplification;
                        if (options.trace) {
                            trace_candidate(plan, logical, candidate_useful,
                                            merged_length, amplification,
                                            "REJECTED: amplification");
                        }
                    } else {
                        merged = true;
                        current.length = merged_length;
                        current.useful_bytes = candidate_useful;
                        current.coverage_end =
                            std::max(current.coverage_end, logical_end);
                        current.slices.push_back(PhysicalSlice{
                            logical_index,
                            static_cast<std::size_t>(logical.offset - current.offset),
                            logical.length,
                        });
                        ++plan.merges_accepted;
                        if (should_deduplicate) {
                            ++plan.requests_deduplicated;
                            ++plan.physical_reads_avoided_by_dedup;
                            plan.bytes_avoided_by_dedup += covered_overlap;
                        } else {
                            ++plan.requests_merged;
                            ++plan.merge_saved_operations;
                        }
                        if (options.trace) {
                            trace_candidate(plan, logical, candidate_useful,
                                            merged_length, amplification,
                                            "ACCEPTED");
                        }
                    }
                }
            }
        }

        if (!merged) {
            plan.requests.push_back(make_physical_request(logical, logical_index));
        }
    }

    return plan;
}

} // namespace intentio::detail
