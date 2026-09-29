#include "detail/policy.hpp"
#include "detail/planner.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

namespace {

struct IntentCase {
    const char* name;
    intentio::GroupHints hints;
};

std::vector<intentio::ReadRequest> make_requests(std::size_t count) {
    std::vector<intentio::ReadRequest> requests;
    requests.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        const std::uint64_t offset =
            static_cast<std::uint64_t>((index % 4096U) * 4096U);
        const std::size_t length = index % 5U == 0U ? 16384U : 4096U;
        requests.push_back(intentio::ReadRequest{
            static_cast<intentio::RequestId>(index + 1),
            3,
            offset,
            length,
        });
    }
    return requests;
}

} // namespace

int main() {
    constexpr std::size_t repetitions = 200;
    const std::vector<std::size_t> group_sizes = {8, 32, 128, 512, 2048, 8192};
    const std::vector<IntentCase> intents = {
        {"Unknown", {}},
        {"Sequential", {intentio::AccessPattern::Sequential,
                         intentio::ReuseHint::Unknown,
                         intentio::Priority::Normal,
                         intentio::LatencyClass::Default}},
        {"Random", {intentio::AccessPattern::Random,
                     intentio::ReuseHint::Unknown,
                     intentio::Priority::Normal,
                     intentio::LatencyClass::Default}},
        {"LatencySensitive", {intentio::AccessPattern::Unknown,
                               intentio::ReuseHint::Unknown,
                               intentio::Priority::Normal,
                               intentio::LatencyClass::LatencySensitive}},
        {"Throughput", {intentio::AccessPattern::Unknown,
                         intentio::ReuseHint::Unknown,
                         intentio::Priority::Normal,
                         intentio::LatencyClass::Throughput}},
    };

    std::cout << "intent,group_size,repetitions,total_ms,ns_per_request,"
                 "physical_requests,merges_accepted,merges_rejected,"
                 "merges_rejected_amplification,"
                 "merges_rejected_size,merges_rejected_gap,requests_merged,"
                 "requests_deduplicated,unique_logical_bytes\n";

    for (const IntentCase& intent : intents) {
        for (const std::size_t group_size : group_sizes) {
            const auto requests = make_requests(group_size);
            intentio::RuntimeOptions runtime_options;
            const auto policy =
                intentio::detail::resolve_policy(runtime_options, intent.hints);
            const intentio::detail::PlannerOptions planner_options{};

            std::size_t physical_requests = 0;
            std::uint64_t accepted = 0;
            std::uint64_t rejected = 0;
            std::uint64_t rejected_amplification = 0;
            std::uint64_t rejected_size = 0;
            std::uint64_t rejected_gap = 0;
            std::uint64_t merged = 0;
            std::uint64_t deduplicated = 0;
            std::uint64_t unique_bytes = 0;
            const auto start = std::chrono::steady_clock::now();
            for (std::size_t repetition = 0; repetition < repetitions; ++repetition) {
                const auto plan = intentio::detail::plan_reads(
                    requests, policy, planner_options);
                physical_requests = plan.requests.size();
                accepted = plan.merges_accepted;
                rejected = plan.merges_rejected;
                rejected_amplification = plan.merges_rejected_amplification;
                rejected_size = plan.merges_rejected_size;
                rejected_gap = plan.merges_rejected_gap;
                merged = plan.requests_merged;
                deduplicated = plan.requests_deduplicated;
                unique_bytes = plan.unique_logical_bytes;
            }
            const auto stop = std::chrono::steady_clock::now();
            const double total_ms =
                std::chrono::duration<double, std::milli>(stop - start).count();
            const double request_count =
                static_cast<double>(group_size * repetitions);
            const double ns_per_request =
                total_ms * 1000000.0 / std::max(1.0, request_count);
            std::cout << intent.name << ',' << group_size << ',' << repetitions << ','
                      << std::fixed << std::setprecision(3) << total_ms << ','
                      << ns_per_request << ',' << physical_requests << ',' << accepted
                      << ',' << rejected << ',' << rejected_amplification << ','
                      << rejected_size << ','
                      << rejected_gap << ',' << merged << ',' << deduplicated << ','
                      << unique_bytes << '\n';
        }
    }
}
