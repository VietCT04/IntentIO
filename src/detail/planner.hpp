#pragma once

#include "intentio/hints.hpp"
#include "intentio/policy.hpp"
#include "intentio/request.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace intentio::detail {

struct PhysicalSlice {
    std::size_t logical_index{};
    std::size_t physical_offset{};
    std::size_t length{};
};

struct PhysicalRequest {
    int fd{-1};
    std::uint64_t offset{};
    std::size_t length{};
    std::vector<PhysicalSlice> slices;
    std::uint64_t useful_bytes{};
    std::uint64_t coverage_end{};
};

struct PhysicalPlan {
    std::vector<PhysicalRequest> requests;
    std::uint64_t requests_merged{};
    std::uint64_t merge_saved_operations{};
    std::uint64_t requests_deduplicated{};
    std::uint64_t physical_reads_avoided_by_dedup{};
    std::uint64_t bytes_avoided_by_dedup{};
    std::uint64_t unique_logical_bytes{};
    std::uint64_t merge_candidates{};
    std::uint64_t merges_accepted{};
    std::uint64_t merges_rejected{};
    std::uint64_t merges_rejected_amplification{};
    std::uint64_t merges_rejected_size{};
    std::uint64_t merges_rejected_gap{};
    std::uint64_t merges_rejected_policy{};
    std::string trace;
};

struct PlannerOptions {
    bool trace{false};
};

PhysicalPlan plan_reads(const std::vector<intentio::ReadRequest>& requests,
                        const intentio::PlanningPolicy& policy,
                        const PlannerOptions& options);

} // namespace intentio::detail
