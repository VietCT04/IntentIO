#include <intentio/intentio.hpp>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <vector>

namespace {

struct Options {
    std::string workload{"sequential"};
    std::string backend{"sync"};
    std::string coalesce{"on"};
    std::string output{};
    std::size_t requests{1024};
    std::size_t block_size{4096};
    std::size_t repetitions{7};
    std::uint64_t gap_bytes{};
};

class TemporaryFile {
public:
    explicit TemporaryFile(std::size_t size) {
        char path[] = "/tmp/intentio-benchmark-XXXXXX";
        fd_ = ::mkstemp(path);
        if (fd_ < 0) {
            throw std::runtime_error(std::string("mkstemp: ") + std::strerror(errno));
        }
        ::unlink(path);
        if (::ftruncate(fd_, static_cast<off_t>(size)) != 0) {
            throw std::runtime_error(std::string("ftruncate: ") + std::strerror(errno));
        }

        constexpr std::size_t chunk_size = 1024 * 1024;
        std::vector<std::byte> chunk(chunk_size);
        for (std::size_t index = 0; index < chunk.size(); ++index) {
            chunk[index] = static_cast<std::byte>(index % 251U);
        }
        std::size_t offset = 0;
        while (offset < size) {
            const std::size_t length = std::min(chunk.size(), size - offset);
            const ssize_t written = ::pwrite(fd_,
                                             chunk.data(),
                                             length,
                                             static_cast<off_t>(offset));
            if (written < 0) {
                if (errno == EINTR) {
                    continue;
                }
                throw std::runtime_error(std::string("pwrite: ") + std::strerror(errno));
            }
            offset += static_cast<std::size_t>(written);
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
              << "  --workload sequential|random|adjacent|mixed\n"
              << "  --backend sync|threaded|io_uring|auto\n"
              << "  --coalesce on|off\n"
              << "  --requests N --block-size BYTES --repetitions N\n"
              << "  --gap BYTES --output FILE\n";
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
    if (options.workload != "sequential" && options.workload != "random" &&
        options.workload != "adjacent" && options.workload != "mixed") {
        throw std::invalid_argument("unknown workload: " + options.workload);
    }
    if (options.backend != "sync" && options.backend != "threaded" &&
        options.backend != "io_uring" &&
        options.backend != "auto") {
        throw std::invalid_argument("unknown backend: " + options.backend);
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

std::vector<std::uint64_t> make_offsets(const Options& options) {
    std::vector<std::uint64_t> offsets;
    offsets.reserve(options.requests);
    std::mt19937_64 generator(0x1A7E710ULL);
    std::uniform_int_distribution<std::uint64_t> distribution(
        0,
        static_cast<std::uint64_t>(options.requests * options.block_size));

    for (std::size_t index = 0; index < options.requests; ++index) {
        if (options.workload == "random" ||
            (options.workload == "mixed" && index % 5U != 0U)) {
            offsets.push_back(distribution(generator));
        } else {
            const std::uint64_t gap =
                (options.workload == "mixed" && index % 5U == 0U) ? options.block_size * 8U : 0U;
            offsets.push_back(static_cast<std::uint64_t>(index * options.block_size) + gap);
        }
    }
    return offsets;
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
        const std::size_t file_size =
            options.requests * options.block_size * 2U + options.block_size * 16U;
        TemporaryFile file(file_size);
        const auto offsets = make_offsets(options);

        intentio::RuntimeOptions runtime_options;
        runtime_options.backend = parse_backend(options.backend);
        runtime_options.queue_depth = 128;
        runtime_options.coalescing.enabled = options.coalesce == "on";
        runtime_options.coalescing.max_gap_bytes = options.gap_bytes;
        runtime_options.coalescing.max_request_bytes = 8U * 1024U * 1024U;
        intentio::Runtime runtime(runtime_options);

        std::vector<double> latencies_ms;
        latencies_ms.reserve(options.repetitions);
        const std::clock_t cpu_start = std::clock();
        for (std::size_t repetition = 0; repetition < options.repetitions; ++repetition) {
            auto group = runtime.create_group();
            if (options.workload == "sequential" || options.workload == "adjacent") {
                group.hint(intentio::AccessPattern::Sequential);
            } else if (options.workload == "random") {
                group.hint(intentio::AccessPattern::Random);
            }
            for (const std::uint64_t offset : offsets) {
                group.read(file.fd(), offset, options.block_size);
            }

            const auto start = std::chrono::steady_clock::now();
            const auto results = group.submit();
            const auto stop = std::chrono::steady_clock::now();
            for (const auto& result : results) {
                if (!result.ok() || result.bytes_read() != options.block_size) {
                    throw std::runtime_error("benchmark read did not return a full block");
                }
            }
            latencies_ms.push_back(std::chrono::duration<double, std::milli>(stop - start).count());
        }
        const double cpu_seconds =
            static_cast<double>(std::clock() - cpu_start) / static_cast<double>(CLOCKS_PER_SEC);
        const auto metrics = runtime.metrics();
        const double median_ms = percentile(latencies_ms, 0.50);
        const double p95_ms = percentile(latencies_ms, 0.95);
        const double p99_ms = percentile(latencies_ms, 0.99);
        const double logical_bytes =
            static_cast<double>(options.requests * options.block_size);
        const double throughput_mib_s =
            logical_bytes / (median_ms / 1000.0) / (1024.0 * 1024.0);

        std::ostream* output = &std::cout;
        std::ofstream file_output;
        if (!options.output.empty()) {
            file_output.open(options.output);
            if (!file_output) {
                throw std::runtime_error("could not open benchmark output: " + options.output);
            }
            output = &file_output;
        }

        *output << "workload,requested_backend,actual_backend,coalescing,repetitions,"
                   "requests,block_size,median_ms,p50_ms,p95_ms,p99_ms,throughput_mib_s,"
                   "cpu_seconds,logical_reads,physical_reads,logical_bytes,physical_bytes,"
                   "requests_merged,merge_saved_operations\n";
        *output << options.workload << ',' << options.backend << ','
                << backend_name(runtime.backend()) << ',' << options.coalesce << ','
                << options.repetitions << ',' << options.requests << ','
                << options.block_size << ',' << std::fixed << std::setprecision(3)
                << median_ms << ',' << median_ms << ',' << p95_ms << ',' << p99_ms << ','
                << throughput_mib_s << ',' << cpu_seconds << ','
                << metrics.logical_reads << ',' << metrics.physical_reads << ','
                << metrics.logical_bytes << ',' << metrics.physical_bytes << ','
                << metrics.requests_merged << ',' << metrics.merge_saved_operations << '\n';
    } catch (const std::exception& error) {
        std::cerr << "benchmark failed: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
