#pragma once

#include "intentio/hints.hpp"
#include "intentio/request.hpp"

#include <cstddef>
#include <cstdint>
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
};

struct PhysicalPlan {
    std::vector<PhysicalRequest> requests;
};

struct PlannerOptions {
    bool enabled{true};
    std::uint64_t max_gap_bytes{};
    std::size_t max_request_bytes{};
};

PhysicalPlan plan_reads(const std::vector<intentio::ReadRequest>& requests,
                        const intentio::GroupHints& hints,
                        const PlannerOptions& options);

} // namespace intentio::detail
