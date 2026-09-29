#include <intentio/intentio.hpp>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <numeric>
#include <random>
#include <stdexcept>
#include <string>
#include <sys/resource.h>
#include <sys/types.h>
#include <unistd.h>
#include <vector>

namespace {

struct Options {
    std::string workload{"random4k"};
    std::string backend{"threaded"};
    std::string mode{"fixed"};
    std::string intent{"unknown"};
    std::string output{};
    std::size_t qd{1};
    std::size_t max_qd{128};
    std::size_t workers{128};
    std::size_t requests{128};
    std::size_t repetitions{5};
};

struct Range {
    std::uint64_t offset{};
    std::size_t length{};
};

struct ResourceUsage {
    double cpu_seconds{};
    long voluntary_context_switches{};
    long involuntary_context_switches{};
};

class TemporaryFile {
public:
    explicit TemporaryFile(std::size_t size) {
        char path[] = "/tmp/intentio-phase6-XXXXXX";
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
              << "  --workload random4k|random64k|sequential|intent|mixed|changing|all\n"
              << "  --backend threaded|io_uring|auto\n"
              << "  --mode fixed|adaptive --intent unknown|sequential|random|latency|throughput|all\n"
              << "  --qd N --max-qd N --workers N --requests N --repetitions N\n"
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
        } else if (argument == "--backend") {
            options.backend = value;
        } else if (argument == "--mode") {
            options.mode = value;
        } else if (argument == "--intent") {
            options.intent = value;
        } else if (argument == "--qd") {
            options.qd = parse_size(value, "qd");
        } else if (argument == "--max-qd") {
            options.max_qd = parse_size(value, "max-qd");
        } else if (argument == "--workers") {
            options.workers = parse_size(value, "workers");
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
        options.workload == "random4k" || options.workload == "random64k" ||
        options.workload == "sequential" || options.workload == "intent" ||
        options.workload == "mixed" || options.workload == "changing" ||
        options.workload == "all";
    if (!valid_workload) {
        throw std::invalid_argument("unknown workload: " + options.workload);
    }
    if (options.mode != "fixed" && options.mode != "adaptive") {
        throw std::invalid_argument("mode must be fixed or adaptive");
    }
    const bool valid_intent =
        options.intent == "unknown" || options.intent == "sequential" ||
        options.intent == "random" || options.intent == "latency" ||
        options.intent == "throughput" || options.intent == "all";
    if (!valid_intent) {
        throw std::invalid_argument("unknown intent: " + options.intent);
    }
    if (options.backend != "threaded" && options.backend != "io_uring" &&
        options.backend != "auto") {
        throw std::invalid_argument("unknown backend: " + options.backend);
    }
    if (options.max_qd < options.qd) {
        throw std::invalid_argument("max-qd must be at least qd");
    }
    return options;
}

intentio::Backend parse_backend(const std::string& backend) {
    if (backend == "io_uring") {
        return intentio::Backend::IoUring;
    }
    if (backend == "auto") {
        return intentio::Backend::Auto;
    }
    return intentio::Backend::ThreadedPread;
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
    constexpr std::size_t four_kib = 4U * 1024U;
    constexpr std::size_t sixty_four_kib = 64U * 1024U;
    constexpr std::size_t one_mib = 1024U * 1024U;
    constexpr std::uint64_t random_stride = 2U * 1024U * 1024U;
    std::vector<Range> ranges;
    ranges.reserve(request_count);

    if (workload == "random4k" || workload == "random64k") {
        const std::size_t length =
            workload == "random4k" ? four_kib : sixty_four_kib;
        std::vector<std::size_t> order(request_count);
        std::iota(order.begin(), order.end(), 0U);
        std::mt19937_64 generator(0x6A11CEULL);
        std::shuffle(order.begin(), order.end(), generator);
        for (const std::size_t index : order) {
            ranges.push_back({static_cast<std::uint64_t>(index) * random_stride,
                              length});
        }
    } else if (workload == "sequential") {
        for (std::size_t index = 0; index < request_count; ++index) {
            ranges.push_back({static_cast<std::uint64_t>(index) * one_mib,
                              one_mib});
        }
    } else if (workload == "intent") {
        constexpr std::uint64_t gap = 16U * 1024U;
        for (std::size_t index = 0; index < request_count; ++index) {
            ranges.push_back({static_cast<std::uint64_t>(index) *
                                  (static_cast<std::uint64_t>(sixty_four_kib) + gap),
                              sixty_four_kib});
        }
    } else if (workload == "mixed") {
        for (std::size_t index = 0; index < request_count; ++index) {
            const std::size_t length =
                index % 10U == 0U ? one_mib
                : (index % 5U == 0U ? sixty_four_kib : four_kib);
            ranges.push_back({static_cast<std::uint64_t>(index) * random_stride,
                              length});
        }
    }
    return ranges;
}

std::size_t required_file_size(const std::vector<Range>& ranges) {
    std::uint64_t end = 0;
    for (const Range& range : ranges) {
        end = std::max(end, range.offset + static_cast<std::uint64_t>(range.length));
    }
    return static_cast<std::size_t>(end + 1024U * 1024U);
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
    }
}

ResourceUsage read_usage() {
    rusage usage{};
    if (::getrusage(RUSAGE_SELF, &usage) != 0) {
        throw std::runtime_error(std::string("getrusage: ") + std::strerror(errno));
    }
    const double user = static_cast<double>(usage.ru_utime.tv_sec) +
                        static_cast<double>(usage.ru_utime.tv_usec) / 1.0e6;
    const double system = static_cast<double>(usage.ru_stime.tv_sec) +
                          static_cast<double>(usage.ru_stime.tv_usec) / 1.0e6;
    return ResourceUsage{user + system, usage.ru_nvcsw, usage.ru_nivcsw};
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

std::string join_qd(const std::vector<std::size_t>& values) {
    std::string result;
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (index != 0) {
            result += '|';
        }
        result += std::to_string(values[index]);
    }
    return result;
}

void emit_header(std::ostream& output) {
    output << "workload,phase,intent,mode,requested_backend,actual_backend,"
              "worker_count,max_in_flight,repetitions,requests,median_ms,p95_ms,p99_ms,"
              "cpu_seconds,voluntary_context_switches,involuntary_context_switches,"
              "logical_reads,physical_reads,physical_bytes,read_amplification,"
              "concurrency_increases,concurrency_decreases,current_concurrency,"
              "peak_concurrency,controller_windows,ewma_completion_latency_ms,"
              "completion_rate,average_concurrency,qd_evolution\n";
}

void emit_case(std::ostream& output,
               const std::string& workload,
               const std::string& phase,
               const std::string& intent,
               const Options& options,
               const std::vector<Range>& ranges,
               int fd) {
    intentio::RuntimeOptions runtime_options;
    runtime_options.backend = parse_backend(options.backend);
    runtime_options.queue_depth =
        options.mode == "adaptive" ? options.max_qd : options.qd;
    runtime_options.threaded_worker_count = options.workers;
    runtime_options.execution.max_in_flight =
        options.mode == "adaptive" ? options.max_qd : options.qd;
    runtime_options.adaptive_execution.enabled = options.mode == "adaptive";
    runtime_options.coalescing.enabled = workload == "intent";
    intentio::Runtime runtime(runtime_options);

    std::vector<double> latencies_ms;
    std::vector<std::size_t> qd_evolution;
    latencies_ms.reserve(options.repetitions);
    qd_evolution.reserve(options.repetitions);
    const ResourceUsage usage_start = read_usage();
    for (std::size_t repetition = 0; repetition < options.repetitions;
         ++repetition) {
        auto group = runtime.create_group();
        apply_intent(group, intent);
        for (const Range& range : ranges) {
            group.read(fd, range.offset, range.length);
        }
        const auto start = std::chrono::steady_clock::now();
        const auto results = group.submit();
        const auto stop = std::chrono::steady_clock::now();
        for (const auto& result : results) {
            if (!result.ok() || result.bytes_read() != result.requested_bytes) {
                throw std::runtime_error("phase 6 read did not complete");
            }
        }
        latencies_ms.push_back(
            std::chrono::duration<double, std::milli>(stop - start).count());
        qd_evolution.push_back(runtime.metrics().current_concurrency);
    }
    const ResourceUsage usage_stop = read_usage();
    const auto metrics = runtime.metrics();
    double average_concurrency = 0.0;
    for (const std::size_t value : qd_evolution) {
        average_concurrency += static_cast<double>(value);
    }
    average_concurrency /= static_cast<double>(qd_evolution.size());

    output << workload << ',' << phase << ',' << intent << ',' << options.mode << ','
            << options.backend << ',' << backend_name(runtime.backend()) << ','
            << options.workers << ','
            << (options.mode == "adaptive" ? options.max_qd : options.qd) << ','
            << options.repetitions << ',' << options.requests << ',' << std::fixed
            << std::setprecision(3) << percentile(latencies_ms, 0.50) << ','
            << percentile(latencies_ms, 0.95) << ','
            << percentile(latencies_ms, 0.99) << ','
            << usage_stop.cpu_seconds - usage_start.cpu_seconds << ','
            << usage_stop.voluntary_context_switches - usage_start.voluntary_context_switches
            << ',' << usage_stop.involuntary_context_switches -
                   usage_start.involuntary_context_switches
            << ',' << metrics.logical_reads << ',' << metrics.physical_reads << ','
            << metrics.physical_bytes << ',' << metrics.read_amplification << ','
            << metrics.concurrency_increases << ',' << metrics.concurrency_decreases << ','
            << metrics.current_concurrency << ',' << metrics.peak_concurrency << ','
            << metrics.controller_windows << ',' << metrics.ewma_completion_latency_ms << ','
            << metrics.completion_rate << ',' << average_concurrency << ','
            << join_qd(qd_evolution) << '\n';
}

void run_changing(std::ostream& output, const Options& options) {
    const auto phase_a = make_ranges("random4k", options.requests);
    const auto phase_b = make_ranges("sequential", options.requests);
    const auto phase_c = make_ranges("random64k", options.requests);
    std::vector<Range> all_ranges = phase_a;
    all_ranges.insert(all_ranges.end(), phase_b.begin(), phase_b.end());
    all_ranges.insert(all_ranges.end(), phase_c.begin(), phase_c.end());
    TemporaryFile file(required_file_size(all_ranges));

    const std::vector<std::pair<std::string, std::string>> phases = {
        {"random4k", "random"},
        {"sequential", "throughput"},
        {"random64k", "random"},
    };
    for (const auto& phase : phases) {
        const auto ranges = make_ranges(phase.first, options.requests);
        emit_case(output, "changing", phase.first, phase.second, options, ranges,
                  file.fd());
    }
}

} // namespace

int main(int argc, char** argv) {
    try {
        const Options options = parse_options(argc, argv);
        const std::vector<std::string> workloads = selected(
            options.workload,
            {"random4k", "random64k", "sequential", "intent", "mixed"});
        const std::vector<std::string> intents = selected(
            options.intent, {"unknown", "sequential", "random", "latency", "throughput"});

        std::ostream* output = &std::cout;
        std::ofstream file_output;
        if (!options.output.empty()) {
            file_output.open(options.output);
            if (!file_output) {
                throw std::runtime_error("could not open output: " + options.output);
            }
            output = &file_output;
        }
        emit_header(*output);

        if (options.workload == "changing") {
            if (options.intent != "unknown") {
                throw std::invalid_argument(
                    "changing workload chooses its intent per phase");
            }
            run_changing(*output, options);
            return 0;
        }

        for (const std::string& workload : workloads) {
            const auto ranges = make_ranges(workload, options.requests);
            TemporaryFile file(required_file_size(ranges));
            for (const std::string& intent : intents) {
                emit_case(*output, workload, workload, intent, options, ranges,
                          file.fd());
            }
        }
    } catch (const std::exception& error) {
        std::cerr << "phase 6 benchmark failed: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
