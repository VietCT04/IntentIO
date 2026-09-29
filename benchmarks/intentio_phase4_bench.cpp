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
    std::string workload{"duplicates"};
    std::string backend{"sync"};
    std::string dedup{"on"};
    std::string coalesce{"on"};
    std::string output{};
    std::size_t requests{256};
    std::size_t block_size{4096};
    std::size_t repetitions{7};
    std::uint64_t gap_bytes{};
};

struct LogicalRange {
    std::uint64_t offset{};
    std::size_t length{};
};

class TemporaryFile {
public:
    explicit TemporaryFile(std::size_t size) {
        char path[] = "/tmp/intentio-phase4-XXXXXX";
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
              << "  --workload duplicates|hotset|nested|partial|nonoverlap|random|mixed\n"
              << "  --backend sync|threaded|io_uring|auto\n"
              << "  --dedup on|off --coalesce on|off\n"
              << "  --requests N --block-size BYTES --repetitions N --gap BYTES\n"
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

std::uint64_t parse_gap(const std::string& value) {
    try {
        std::size_t consumed = 0;
        const auto parsed = std::stoull(value, &consumed, 10);
        if (consumed != value.size()) {
            throw std::invalid_argument("not an integer");
        }
        return static_cast<std::uint64_t>(parsed);
    } catch (const std::exception&) {
        throw std::invalid_argument("invalid gap: " + value);
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
        } else if (argument == "--dedup") {
            options.dedup = value;
        } else if (argument == "--coalesce") {
            options.coalesce = value;
        } else if (argument == "--requests") {
            options.requests = parse_size(value, "requests");
        } else if (argument == "--block-size") {
            options.block_size = parse_size(value, "block-size");
        } else if (argument == "--repetitions") {
            options.repetitions = parse_size(value, "repetitions");
        } else if (argument == "--gap") {
            options.gap_bytes = parse_gap(value);
        } else if (argument == "--output") {
            options.output = value;
        } else {
            throw std::invalid_argument("unknown option: " + argument);
        }
    }

    const bool valid_workload =
        options.workload == "duplicates" || options.workload == "hotset" ||
        options.workload == "nested" || options.workload == "partial" ||
        options.workload == "nonoverlap" || options.workload == "random" ||
        options.workload == "mixed";
    if (!valid_workload) {
        throw std::invalid_argument("unknown workload: " + options.workload);
    }
    if (options.backend != "sync" && options.backend != "threaded" &&
        options.backend != "io_uring" &&
        options.backend != "auto") {
        throw std::invalid_argument("unknown backend: " + options.backend);
    }
    if (options.dedup != "on" && options.dedup != "off") {
        throw std::invalid_argument("dedup must be on or off");
    }
    if (options.coalesce != "on" && options.coalesce != "off") {
        throw std::invalid_argument("coalesce must be on or off");
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

std::vector<LogicalRange> make_ranges(const Options& options) {
    std::vector<LogicalRange> ranges;
    ranges.reserve(options.requests);
    std::mt19937_64 generator(0x482193ULL);
    const std::uint64_t block = static_cast<std::uint64_t>(options.block_size);
    const std::uint64_t random_limit =
        static_cast<std::uint64_t>(options.requests * options.block_size * 2U);
    std::uniform_int_distribution<std::uint64_t> random_offset(0, random_limit);
    std::uniform_int_distribution<std::size_t> hot_block(0, 63);

    for (std::size_t index = 0; index < options.requests; ++index) {
        if (options.workload == "duplicates") {
            ranges.push_back({0, options.block_size});
        } else if (options.workload == "hotset") {
            ranges.push_back({hot_block(generator) * block, options.block_size});
        } else if (options.workload == "nested") {
            if (index == 0) {
                ranges.push_back({0, 1024U * 1024U});
            } else {
                const std::size_t choices[] = {4096, 16384, 65536};
                const std::size_t length = choices[index % 3U];
                const std::uint64_t limit = 1024U * 1024U - length;
                ranges.push_back({(index * 65536U) % limit, length});
            }
        } else if (options.workload == "partial") {
            ranges.push_back({index * block, options.block_size * 2U});
        } else if (options.workload == "nonoverlap") {
            ranges.push_back({index * block * 3U, options.block_size});
        } else if (options.workload == "random") {
            ranges.push_back({random_offset(generator), options.block_size});
        } else {
            switch (index % 10U) {
            case 0:
            case 1:
            case 2:
            case 3:
            case 4:
                ranges.push_back({random_offset(generator), options.block_size});
                break;
            case 5:
            case 6:
                ranges.push_back({0, options.block_size});
                break;
            case 7:
            case 8:
                ranges.push_back({4U * 1024U * 1024U + (index / 10U) * block,
                                  options.block_size * 2U});
                break;
            default:
                ranges.push_back({8U * 1024U * 1024U + (index / 10U) * block,
                                  options.block_size});
                break;
            }
        }
    }
    return ranges;
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

} // namespace

int main(int argc, char** argv) {
    try {
        const Options options = parse_options(argc, argv);
        const std::size_t file_size = std::max(
            static_cast<std::size_t>(16U * 1024U * 1024U),
            options.requests * options.block_size * 4U + 2U * 1024U * 1024U);
        TemporaryFile file(file_size);
        const auto ranges = make_ranges(options);

        intentio::RuntimeOptions runtime_options;
        runtime_options.backend = parse_backend(options.backend);
        runtime_options.queue_depth = 128;
        runtime_options.coalescing.enabled = options.coalesce == "on";
        runtime_options.coalescing.max_gap_bytes = options.gap_bytes;
        runtime_options.coalescing.max_request_bytes = 8U * 1024U * 1024U;
        runtime_options.deduplication.enabled = options.dedup == "on";
        intentio::Runtime runtime(runtime_options);

        std::vector<double> latencies_ms;
        latencies_ms.reserve(options.repetitions);
        std::uint64_t result_copy_bytes = 0;
        const std::clock_t cpu_start = std::clock();
        for (std::size_t repetition = 0; repetition < options.repetitions; ++repetition) {
            auto group = runtime.create_group();
            if (options.workload == "random" || options.workload == "hotset") {
                group.hint(intentio::AccessPattern::Random);
            } else {
                group.hint(intentio::AccessPattern::Sequential);
            }
            for (const LogicalRange& range : ranges) {
                group.read(file.fd(), range.offset, range.length);
            }

            const auto start = std::chrono::steady_clock::now();
            const auto results = group.submit();
            const auto stop = std::chrono::steady_clock::now();
            for (const auto& result : results) {
                if (!result.ok() || result.bytes_read() != result.requested_bytes) {
                    throw std::runtime_error("phase 4 benchmark read was not complete");
                }
                result_copy_bytes += result.bytes_read();
            }
            latencies_ms.push_back(
                std::chrono::duration<double, std::milli>(stop - start).count());
        }

        const auto metrics = runtime.metrics();
        const double cpu_seconds =
            static_cast<double>(std::clock() - cpu_start) /
            static_cast<double>(CLOCKS_PER_SEC);
        const double median_ms = percentile(latencies_ms, 0.50);
        const double p95_ms = percentile(latencies_ms, 0.95);
        const double p99_ms = percentile(latencies_ms, 0.99);
        const double throughput_mib_s =
            static_cast<double>(options.requests * options.block_size) /
            (median_ms / 1000.0) / (1024.0 * 1024.0);

        std::ostream* output = &std::cout;
        std::ofstream file_output;
        if (!options.output.empty()) {
            file_output.open(options.output);
            if (!file_output) {
                throw std::runtime_error("could not open output: " + options.output);
            }
            output = &file_output;
        }

        *output << "workload,requested_backend,actual_backend,dedup,coalescing,max_gap_bytes,"
                   "repetitions,requests,block_size,median_ms,p50_ms,p95_ms,p99_ms,throughput_mib_s,"
                   "cpu_seconds,result_copy_bytes,logical_reads,physical_reads,logical_bytes,"
                   "unique_logical_bytes,physical_bytes,requests_merged,requests_deduplicated,"
                   "physical_reads_avoided_by_dedup,bytes_avoided_by_dedup,read_amplification\n";
        *output << options.workload << ',' << options.backend << ','
                << backend_name(runtime.backend()) << ',' << options.dedup << ','
                << options.coalesce << ',' << options.gap_bytes << ',' << options.repetitions
                << ',' << options.requests
                << ',' << options.block_size << ',' << std::fixed << std::setprecision(3)
                << median_ms << ',' << median_ms << ',' << p95_ms << ',' << p99_ms << ','
                << throughput_mib_s << ',' << cpu_seconds << ',' << result_copy_bytes << ','
                << metrics.logical_reads << ',' << metrics.physical_reads << ','
                << metrics.logical_bytes << ',' << metrics.unique_logical_bytes << ','
                << metrics.physical_bytes << ',' << metrics.requests_merged << ','
                << metrics.requests_deduplicated << ','
                << metrics.physical_reads_avoided_by_dedup << ','
                << metrics.bytes_avoided_by_dedup << ',' << metrics.read_amplification << '\n';
    } catch (const std::exception& error) {
        std::cerr << "phase 4 benchmark failed: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
