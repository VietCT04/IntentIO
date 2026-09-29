#include <intentio/intentio.hpp>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>
#include <string>
#include <sys/types.h>
#include <unistd.h>
#include <vector>

namespace {

struct Options {
    std::string workload{"sequential"};
    std::string intent{"all"};
    std::string backend{"sync"};
    std::string output{};
    std::size_t requests{128};
    std::size_t repetitions{7};
};

struct Range {
    std::uint64_t offset{};
    std::size_t length{};
};

class TemporaryFile {
public:
    explicit TemporaryFile(std::size_t size) {
        char path[] = "/tmp/intentio-phase5-XXXXXX";
        fd_ = ::mkstemp(path);
        if (fd_ < 0) {
            throw std::runtime_error(std::string("mkstemp: ") + std::strerror(errno));
        }
        ::unlink(path);
        if (::ftruncate(fd_, static_cast<off_t>(size)) != 0) {
            throw std::runtime_error(std::string("ftruncate: ") + std::strerror(errno));
        }
    }

    ~TemporaryFile() {
        if (fd_ >= 0) {
            ::close(fd_);
        }
    }

    TemporaryFile(const TemporaryFile&) = delete;
    TemporaryFile& operator=(const TemporaryFile&) = delete;

    [[nodiscard]] int fd() const noexcept { return fd_; }

private:
    int fd_{-1};
};

void usage(const char* program) {
    std::cout << "usage: " << program << " [options]\n"
              << "  --workload sequential|random|latency|throughput|misuse|all\n"
              << "  --intent unknown|sequential|random|latency|throughput|scan_once|reuse_likely|seq_throughput|all\n"
              << "  --backend sync|threaded|io_uring|auto --requests N --repetitions N\n"
              << "  --output FILE\n";
}

std::size_t parse_size(const std::string& value, const char* name) {
    try {
        std::size_t consumed = 0;
        const auto parsed = std::stoull(value, &consumed, 10);
        if (consumed != value.size() || parsed == 0 ||
            parsed > std::numeric_limits<std::size_t>::max()) {
            throw std::invalid_argument("not positive");
        }
        return static_cast<std::size_t>(parsed);
    } catch (const std::exception&) {
        throw std::invalid_argument(std::string("invalid ") + name + ": " + value);
    }
}

Options parse_options(int argc, char** argv) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--help") {
            usage(argv[0]);
            std::exit(0);
        }
        if (index + 1 >= argc) {
            throw std::invalid_argument("missing value for " + argument);
        }
        const std::string value = argv[++index];
        if (argument == "--workload") {
            options.workload = value;
        } else if (argument == "--intent") {
            options.intent = value;
        } else if (argument == "--backend") {
            options.backend = value;
        } else if (argument == "--requests") {
            options.requests = parse_size(value, "requests");
        } else if (argument == "--repetitions") {
            options.repetitions = parse_size(value, "repetitions");
        } else if (argument == "--output") {
            options.output = value;
        } else {
            throw std::invalid_argument("unknown option: " + argument);
        }
    }
    const bool valid_workload =
        options.workload == "sequential" || options.workload == "random" ||
        options.workload == "latency" || options.workload == "throughput" ||
        options.workload == "misuse" || options.workload == "all";
    if (!valid_workload) {
        throw std::invalid_argument("unknown workload: " + options.workload);
    }
    const bool valid_intent =
        options.intent == "unknown" || options.intent == "sequential" ||
        options.intent == "random" || options.intent == "latency" ||
        options.intent == "throughput" || options.intent == "scan_once" ||
        options.intent == "reuse_likely" || options.intent == "seq_throughput" ||
        options.intent == "all";
    if (!valid_intent) {
        throw std::invalid_argument("unknown intent: " + options.intent);
    }
    if (options.backend != "sync" && options.backend != "threaded" &&
        options.backend != "io_uring" &&
        options.backend != "auto") {
        throw std::invalid_argument("unknown backend: " + options.backend);
    }
    return options;
}

intentio::Backend parse_backend(const std::string& backend) {
    if (backend == "threaded") {
        return intentio::Backend::ThreadedPread;
    }
    if (backend == "io_uring") {
        return intentio::Backend::IoUring;
    }
    if (backend == "auto") {
        return intentio::Backend::Auto;
    }
    return intentio::Backend::Synchronous;
}

std::vector<std::string> selected(const std::string& value,
                                  const std::vector<std::string>& all) {
    if (value == "all") {
        return all;
    }
    return {value};
}

std::vector<Range> make_ranges(const std::string& workload,
                               std::size_t request_count) {
    constexpr std::size_t small = 4U * 1024U;
    constexpr std::size_t medium = 64U * 1024U;
    constexpr std::size_t large = 256U * 1024U;
    constexpr std::uint64_t sequential_gap = 16U * 1024U;
    std::vector<Range> ranges;
    ranges.reserve(request_count);
    std::mt19937_64 generator(0x5EED005ULL);

    if (workload == "sequential" || workload == "throughput") {
        const std::size_t length = medium;
        for (std::size_t index = 0; index < request_count; ++index) {
            ranges.push_back({
                static_cast<std::uint64_t>(index) *
                    (static_cast<std::uint64_t>(length) + sequential_gap),
                length,
            });
        }
    } else if (workload == "latency") {
        ranges.push_back({0, small});
        for (std::size_t index = 1; index < request_count; ++index) {
            ranges.push_back({
                16U * 1024U + static_cast<std::uint64_t>(index - 1) *
                    (static_cast<std::uint64_t>(large) + sequential_gap),
                large,
            });
        }
    } else {
        for (std::size_t index = 0; index < request_count; ++index) {
            if (workload == "misuse") {
                ranges.push_back({
                    static_cast<std::uint64_t>(index) *
                        (static_cast<std::uint64_t>(small) + 32U * 1024U),
                    small,
                });
            } else if (workload == "random") {
                ranges.push_back({static_cast<std::uint64_t>(index) * 2U * 1024U * 1024U,
                                  small});
            } else {
                const std::uint64_t offset =
                    (generator() % 4096U) * static_cast<std::uint64_t>(small);
                ranges.push_back({offset, small});
            }
        }
    }
    return ranges;
}

void apply_intent(intentio::Group& group, const std::string& intent) {
    if (intent == "sequential") {
        group.access_pattern(intentio::AccessPattern::Sequential);
    } else if (intent == "random") {
        group.access_pattern(intentio::AccessPattern::Random);
    } else if (intent == "latency") {
        group.latency_class(intentio::LatencyClass::LatencySensitive);
    } else if (intent == "throughput") {
        group.latency_class(intentio::LatencyClass::Throughput);
    } else if (intent == "scan_once") {
        group.reuse_hint(intentio::ReuseHint::ScanOnce);
    } else if (intent == "reuse_likely") {
        group.reuse_hint(intentio::ReuseHint::ReuseLikely);
    } else if (intent == "seq_throughput") {
        group.access_pattern(intentio::AccessPattern::Sequential);
        group.latency_class(intentio::LatencyClass::Throughput);
    }
}

double percentile(std::vector<double> values, double quantile) {
    std::sort(values.begin(), values.end());
    const std::size_t index = static_cast<std::size_t>(
        quantile * static_cast<double>(values.size() - 1));
    return values[index];
}

const char* backend_name(intentio::Backend backend) {
    switch (backend) {
    case intentio::Backend::Synchronous:
        return "sync";
    case intentio::Backend::ThreadedPread:
        return "threaded";
    case intentio::Backend::IoUring:
        return "io_uring";
    case intentio::Backend::Auto:
        return "auto";
    }
    return "unknown";
}

std::size_t required_file_size(const std::vector<Range>& ranges) {
    std::uint64_t end = 0;
    for (const Range& range : ranges) {
        end = std::max(end, range.offset + static_cast<std::uint64_t>(range.length));
    }
    return static_cast<std::size_t>(end + 1024U * 1024U);
}

} // namespace

int main(int argc, char** argv) {
    try {
        const Options options = parse_options(argc, argv);
        const std::vector<std::string> workloads = selected(
            options.workload,
            {"sequential", "random", "latency", "throughput", "misuse"});
        const std::vector<std::string> intents = selected(
            options.intent,
            {"unknown", "sequential", "random", "latency", "throughput",
             "scan_once", "reuse_likely", "seq_throughput"});

        std::ostream* output = &std::cout;
        std::ofstream file_output;
        if (!options.output.empty()) {
            file_output.open(options.output);
            if (!file_output) {
                throw std::runtime_error("could not open output: " + options.output);
            }
            output = &file_output;
        }
        *output << "workload,intent,requested_backend,actual_backend,repetitions,requests,"
                   "median_ms,p50_ms,p95_ms,p99_ms,cpu_seconds,logical_reads,"
                   "physical_reads,logical_bytes,unique_logical_bytes,physical_bytes,"
                   "read_amplification,merge_candidates,merges_accepted,merges_rejected,"
                   "merges_rejected_amplification,merges_rejected_size,merges_rejected_gap,"
                   "merges_rejected_policy,intent_sequential_groups,intent_random_groups,"
                   "latency_sensitive_groups,throughput_groups\n";

        for (const std::string& workload : workloads) {
            const auto ranges = make_ranges(workload, options.requests);
            TemporaryFile file(required_file_size(ranges));
            for (const std::string& intent : intents) {
                intentio::RuntimeOptions runtime_options;
                runtime_options.backend = parse_backend(options.backend);
                runtime_options.queue_depth = 128;
                intentio::Runtime runtime(runtime_options);
                std::vector<double> latencies_ms;
                latencies_ms.reserve(options.repetitions);
                const std::clock_t cpu_start = std::clock();

                for (std::size_t repetition = 0; repetition < options.repetitions;
                     ++repetition) {
                    auto group = runtime.create_group();
                    apply_intent(group, intent);
                    for (const Range& range : ranges) {
                        group.read(file.fd(), range.offset, range.length);
                    }
                    const auto start = std::chrono::steady_clock::now();
                    const auto results = group.submit();
                    const auto stop = std::chrono::steady_clock::now();
                    for (const auto& result : results) {
                        if (!result.ok() || result.bytes_read() != result.requested_bytes) {
                            throw std::runtime_error("intent benchmark read was not complete");
                        }
                    }
                    latencies_ms.push_back(
                        std::chrono::duration<double, std::milli>(stop - start).count());
                }

                const auto metrics = runtime.metrics();
                const double cpu_seconds =
                    static_cast<double>(std::clock() - cpu_start) /
                    static_cast<double>(CLOCKS_PER_SEC);
                const double median_ms = percentile(latencies_ms, 0.50);
                *output << workload << ',' << intent << ',' << options.backend << ','
                        << backend_name(runtime.backend()) << ',' << options.repetitions << ','
                        << options.requests << ',' << std::fixed << std::setprecision(3)
                        << median_ms << ',' << median_ms << ','
                        << percentile(latencies_ms, 0.95) << ','
                        << percentile(latencies_ms, 0.99) << ',' << cpu_seconds << ','
                        << metrics.logical_reads << ',' << metrics.physical_reads << ','
                        << metrics.logical_bytes << ',' << metrics.unique_logical_bytes << ','
                        << metrics.physical_bytes << ',' << metrics.read_amplification << ','
                        << metrics.merge_candidates << ',' << metrics.merges_accepted << ','
                        << metrics.merges_rejected << ','
                        << metrics.merges_rejected_amplification << ','
                        << metrics.merges_rejected_size << ','
                        << metrics.merges_rejected_gap << ','
                        << metrics.merges_rejected_policy << ','
                        << metrics.intent_sequential_groups << ','
                        << metrics.intent_random_groups << ','
                        << metrics.latency_sensitive_groups << ','
                        << metrics.throughput_groups << '\n';
            }
        }
    } catch (const std::exception& error) {
        std::cerr << "phase 5 benchmark failed: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
