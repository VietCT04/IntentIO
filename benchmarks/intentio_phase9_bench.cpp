#include <intentio/intentio.hpp>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <linux/fs.h>
#include <numeric>
#include <random>
#include <stdexcept>
#include <string>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/uio.h>
#include <sys/utsname.h>
#include <sstream>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

#ifndef RWF_DONTCACHE
#define RWF_DONTCACHE 0x00000080U
#endif

namespace {

using Clock = std::chrono::steady_clock;

struct Options {
    std::string mode{"scan"};
    std::string policy{"baseline"};
    std::string workload{"sequential"};
    std::string output{};
    std::size_t qd{16};
    std::size_t workers{16};
    std::size_t requests{128};
    std::size_t hot_requests{4096};
    std::size_t hot_mib{16};
    std::size_t scan_mib{128};
    std::size_t request_size{};
    std::size_t repetitions{1};
    bool materialize_file{false};
};

struct Range {
    std::uint64_t offset{};
    std::size_t length{};
};

struct ResourceUsage {
    double cpu_seconds{};
    long minor_faults{};
    long major_faults{};
};

struct Pending {
    intentio::Submission submission;
    Clock::time_point submitted_at;
};

struct RunResult {
    std::vector<double> completion_ms;
    double wall_ms{};
    ResourceUsage usage{};
    intentio::MetricsSnapshot before{};
    intentio::MetricsSnapshot after{};
};

struct Residency {
    std::size_t resident{};
    std::size_t pages{};
};

class TemporaryFile {
public:
    TemporaryFile(std::size_t size, bool materialize) {
        char path[] = "./intentio-phase9-XXXXXX";
        fd_ = ::mkstemp(path);
        if (fd_ < 0) {
            throw std::runtime_error(std::string("mkstemp: ") + std::strerror(errno));
        }
        ::unlink(path);
        if (::ftruncate(fd_, static_cast<off_t>(size)) != 0) {
            throw std::runtime_error(std::string("ftruncate: ") + std::strerror(errno));
        }
        if (materialize) {
            constexpr std::size_t chunk_size = 1024U * 1024U;
            std::vector<char> chunk(chunk_size, 'I');
            std::size_t written = 0;
            while (written < size) {
                const std::size_t count = std::min(chunk.size(), size - written);
                const ssize_t result = ::pwrite(
                    fd_, chunk.data(), count, static_cast<off_t>(written));
                if (result < 0) {
                    if (errno == EINTR) {
                        continue;
                    }
                    throw std::runtime_error(
                        std::string("pwrite: ") + std::strerror(errno));
                }
                if (result == 0) {
                    throw std::runtime_error("pwrite made no progress");
                }
                written += static_cast<std::size_t>(result);
            }
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
              << "  --mode scan|cache-pollution\n"
              << "  --policy baseline|sequential|random|noreuse|dontcache\n"
              << "  --workload sequential|random4k|random64k|mixed\n"
              << "  --qd N --workers N --requests N --request-size N\n"
              << "  --repetitions N --materialize-file --output FILE\n";
}

std::size_t parse_size(const std::string& value, const char* name) {
    try {
        std::size_t consumed = 0;
        const auto parsed = std::stoull(value, &consumed, 10);
        if (consumed != value.size() || parsed == 0) {
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
        if (argument == "--materialize-file") {
            options.materialize_file = true;
            continue;
        }
        if (index + 1 >= argc) {
            throw std::invalid_argument("missing value for " + argument);
        }
        const std::string value = argv[++index];
        if (argument == "--mode") {
            options.mode = value;
        } else if (argument == "--policy") {
            options.policy = value;
        } else if (argument == "--workload") {
            options.workload = value;
        } else if (argument == "--qd") {
            options.qd = parse_size(value, "qd");
        } else if (argument == "--workers") {
            options.workers = parse_size(value, "workers");
        } else if (argument == "--requests") {
            options.requests = parse_size(value, "requests");
        } else if (argument == "--hot-requests") {
            options.hot_requests = parse_size(value, "hot-requests");
        } else if (argument == "--hot-mib") {
            options.hot_mib = parse_size(value, "hot-mib");
        } else if (argument == "--scan-mib") {
            options.scan_mib = parse_size(value, "scan-mib");
        } else if (argument == "--request-size") {
            options.request_size = parse_size(value, "request-size");
        } else if (argument == "--repetitions") {
            options.repetitions = parse_size(value, "repetitions");
        } else if (argument == "--output") {
            options.output = value;
        } else {
            throw std::invalid_argument("unknown option: " + argument);
        }
    }
    if (options.mode != "scan" && options.mode != "cache-pollution") {
        throw std::invalid_argument("mode must be scan or cache-pollution");
    }
    if (options.policy != "baseline" && options.policy != "sequential" &&
        options.policy != "random" && options.policy != "noreuse" &&
        options.policy != "dontcache") {
        throw std::invalid_argument("unknown policy: " + options.policy);
    }
    if (options.workload != "sequential" && options.workload != "random4k" &&
        options.workload != "random64k" && options.workload != "mixed") {
        throw std::invalid_argument("unknown workload: " + options.workload);
    }
    return options;
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
    return ResourceUsage{user + system, usage.ru_minflt, usage.ru_majflt};
}

double percentile(std::vector<double> values, double quantile) {
    if (values.empty()) {
        return 0.0;
    }
    std::sort(values.begin(), values.end());
    const std::size_t index = static_cast<std::size_t>(
        quantile * static_cast<double>(values.size() - 1));
    return values[index];
}

std::vector<Range> make_ranges(const Options& options,
                               std::size_t request_count,
                               std::size_t file_size) {
    const std::size_t length = options.request_size != 0
                                   ? options.request_size
                                   : options.workload == "random4k" ? 4096U
                                   : options.workload == "random64k" ? 65536U
                                   : options.workload == "mixed" ? 65536U
                                   : 1024U * 1024U;
    const std::uint64_t stride =
        options.workload == "sequential" ? static_cast<std::uint64_t>(length)
                                          : 2U * 1024U * 1024U;
    if (file_size < length || request_count == 0 ||
        request_count - 1U >
            (file_size - length) / static_cast<std::size_t>(stride)) {
        throw std::invalid_argument("benchmark file is too small for workload");
    }

    std::vector<Range> ranges;
    ranges.reserve(request_count);
    std::mt19937_64 generator(0x9E3779B97F4A7C15ULL);
    for (std::size_t index = 0; index < request_count; ++index) {
        std::size_t actual_length = length;
        if (options.workload == "mixed") {
            actual_length = index % 4U == 0U ? 1024U * 1024U
                           : index % 2U == 0U ? 65536U
                                              : 4096U;
        }
        const std::uint64_t offset =
            options.workload == "sequential"
                ? static_cast<std::uint64_t>(index) * stride
                : static_cast<std::uint64_t>(index) * 2U * 1024U * 1024U;
        ranges.push_back({offset, actual_length});
    }
    if (options.workload == "random4k" || options.workload == "random64k" ||
        options.workload == "mixed") {
        std::shuffle(ranges.begin(), ranges.end(), generator);
    }
    return ranges;
}

std::size_t file_size_for(const Options& options, std::size_t requests) {
    const std::size_t length = options.request_size != 0
                                   ? options.request_size
                                   : options.workload == "random4k" ? 4096U
                                   : options.workload == "random64k" ? 65536U
                                   : options.workload == "mixed" ? 1024U * 1024U
                                                                  : 1024U * 1024U;
    const std::uint64_t stride =
        options.workload == "sequential" ? static_cast<std::uint64_t>(length)
                                          : 2U * 1024U * 1024U;
    std::uint64_t end = 0;
    for (std::size_t index = 0; index < requests; ++index) {
        std::size_t actual_length = length;
        if (options.workload == "mixed") {
            actual_length = index % 4U == 0U ? 1024U * 1024U
                           : index % 2U == 0U ? 65536U
                                              : 4096U;
        }
        const std::uint64_t offset =
            options.workload == "sequential"
                ? static_cast<std::uint64_t>(index) * stride
                : static_cast<std::uint64_t>(index) * 2U * 1024U * 1024U;
        end = std::max(end, offset + static_cast<std::uint64_t>(actual_length));
    }
    return static_cast<std::size_t>(end + 1024U * 1024U);
}

std::size_t nominal_request_size(const Options& options) {
    if (options.request_size != 0) {
        return options.request_size;
    }
    if (options.workload == "random4k") {
        return 4096U;
    }
    if (options.workload == "random64k") {
        return 65536U;
    }
    return 1024U * 1024U;
}

void configure_runtime_policy(const Options& options,
                              intentio::RuntimeOptions& runtime_options) {
    runtime_options.kernel_io.enabled = options.policy != "baseline";
    if (options.policy == "noreuse") {
        runtime_options.kernel_io.enable_dontcache = false;
    }
}

void apply_group_policy(const Options& options, intentio::Group& group) {
    if (options.policy == "sequential") {
        group.access_pattern(intentio::AccessPattern::Sequential);
    } else if (options.policy == "random") {
        group.access_pattern(intentio::AccessPattern::Random);
    } else if (options.policy == "noreuse" || options.policy == "dontcache") {
        group.reuse_hint(intentio::ReuseHint::ScanOnce);
    }
}

RunResult run_batch(const Options& options,
                    int fd,
                    const std::vector<Range>& ranges) {
    intentio::RuntimeOptions runtime_options;
    runtime_options.backend = intentio::Backend::ThreadedPread;
    runtime_options.threaded_worker_count = options.workers;
    runtime_options.execution.max_in_flight = options.qd;
    runtime_options.coalescing.enabled = false;
    runtime_options.deduplication.enabled = false;
    configure_runtime_policy(options, runtime_options);
    intentio::Runtime runtime(runtime_options);
    RunResult result;
    result.before = runtime.metrics();
    result.completion_ms.reserve(ranges.size());
    std::vector<Pending> pending;
    pending.reserve(ranges.size());
    const ResourceUsage usage_start = read_usage();
    const auto wall_start = Clock::now();
    for (const Range& range : ranges) {
        auto group = runtime.create_group();
        group.read(fd, range.offset, range.length);
        apply_group_policy(options, group);
        pending.push_back(Pending{runtime.submit(group), Clock::now()});
    }
    for (Pending& item : pending) {
        const auto values = item.submission.get();
        if (values.size() != 1U || !values.front().ok()) {
            throw std::runtime_error("Phase 9 benchmark read failed");
        }
        const auto completed_at = Clock::now();
        result.completion_ms.push_back(
            std::chrono::duration<double, std::milli>(completed_at - item.submitted_at)
                .count());
    }
    const ResourceUsage usage_stop = read_usage();
    result.wall_ms = std::chrono::duration<double, std::milli>(
                         Clock::now() - wall_start)
                         .count();
    result.usage = ResourceUsage{
        usage_stop.cpu_seconds - usage_start.cpu_seconds,
        usage_stop.minor_faults - usage_start.minor_faults,
        usage_stop.major_faults - usage_start.major_faults,
    };
    result.after = runtime.metrics();
    return result;
}

Residency residency(int fd, std::size_t bytes) {
    const long page_size = ::sysconf(_SC_PAGESIZE);
    if (page_size <= 0 || bytes == 0) {
        return {};
    }
    void* mapping = ::mmap(nullptr,
                           bytes,
                           PROT_READ,
                           MAP_SHARED,
                           fd,
                           0);
    if (mapping == MAP_FAILED) {
        return {};
    }
    const std::size_t pages =
        (bytes + static_cast<std::size_t>(page_size) - 1U) /
        static_cast<std::size_t>(page_size);
    std::vector<unsigned char> state(pages);
    Residency result{0, pages};
    if (::mincore(mapping, bytes, state.data()) == 0) {
        result.resident = static_cast<std::size_t>(
            std::count_if(state.begin(), state.end(), [](unsigned char value) {
                return (value & 1U) != 0U;
            }));
    }
    ::munmap(mapping, bytes);
    return result;
}

bool probe_dontcache(int fd) {
    std::vector<char> buffer(4096);
    iovec vector{buffer.data(), buffer.size()};
    const ssize_t result = ::preadv2(fd, &vector, 1, 0, RWF_DONTCACHE);
    return result >= 0;
}

std::string kernel_name() {
    utsname info{};
    return ::uname(&info) == 0 ? info.release : "unknown";
}

std::string filesystem_name(const std::string& path) {
    struct statfs info{};
    if (::statfs(path.c_str(), &info) != 0) {
        return "unknown";
    }
    return "0x" + [&] {
        std::ostringstream value;
        value << std::hex << static_cast<unsigned long long>(info.f_type);
        return value.str();
    }();
}

void emit_header(std::ostream& output) {
    output << "mode,policy,workload,status,kernel,filesystem,file_state,cache_state,"
              "workers,qd,requests,request_size,wall_ms,tasks_per_sec,p50_ms,p95_ms,p99_ms,"
              "cpu_seconds,minor_faults,major_faults,logical_reads,physical_reads,"
              "kernel_advice_sequential,kernel_advice_random,kernel_advice_noreuse,"
              "kernel_advice_failures,dontcache_attempted,dontcache_succeeded,"
              "dontcache_unsupported,dontcache_fallbacks,buffered_reads,"
              "bytes_dontcache,bytes_buffered,hot_p50_before_ms,hot_p50_after_ms,"
              "hot_residency_before,hot_residency_after\n";
}

void emit_scan(std::ostream& output, const Options& options) {
    if (options.policy == "dontcache") {
        // The benchmark is strict for this policy: unsupported means SKIP, not
        // a row that could be mistaken for a successful DONTCACHE run.
        TemporaryFile probe_file(4096, true);
        if (!probe_dontcache(probe_file.fd())) {
            output << "scan," << options.policy << ',' << options.workload
                   << ",SKIP," << kernel_name() << ',' << filesystem_name(".")
                   << ',' << (options.materialize_file ? "materialized" : "sparse")
                   << ",warm-buffered," << options.workers << ',' << options.qd
                   << ',' << options.requests << ',' << nominal_request_size(options)
                   << ",0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0\n";
            return;
        }
    }
    const std::size_t file_size = file_size_for(options, options.requests);
    TemporaryFile file(file_size, options.materialize_file);
    const auto ranges = make_ranges(options, options.requests, file_size);
    std::vector<double> p50;
    std::vector<double> p95;
    std::vector<double> p99;
    RunResult last;
    for (std::size_t repetition = 0; repetition < options.repetitions; ++repetition) {
        last = run_batch(options, file.fd(), ranges);
        p50.push_back(percentile(last.completion_ms, 0.50));
        p95.push_back(percentile(last.completion_ms, 0.95));
        p99.push_back(percentile(last.completion_ms, 0.99));
    }
    const auto& metrics = last.after;
    const double tasks = static_cast<double>(
        metrics.executor_tasks_completed - last.before.executor_tasks_completed);
    const double seconds = last.wall_ms / 1000.0;
    output << "scan," << options.policy << ',' << options.workload << ",OK,"
           << kernel_name() << ',' << filesystem_name(".") << ','
           << (options.materialize_file ? "materialized" : "sparse")
           << ",warm-buffered," << options.workers << ',' << options.qd << ','
           << options.requests << ',' << nominal_request_size(options) << ','
           << std::fixed << std::setprecision(3)
           << last.wall_ms << ',' << (seconds == 0.0 ? 0.0 : tasks / seconds)
           << ',' << percentile(p50, 0.50) << ',' << percentile(p95, 0.95) << ','
           << percentile(p99, 0.99) << ',' << last.usage.cpu_seconds << ','
           << last.usage.minor_faults << ',' << last.usage.major_faults << ','
           << metrics.logical_reads - last.before.logical_reads << ','
           << metrics.physical_reads - last.before.physical_reads << ','
           << metrics.kernel_advice_sequential << ','
           << metrics.kernel_advice_random << ','
           << metrics.kernel_advice_noreuse << ','
           << metrics.kernel_advice_failures << ','
           << metrics.dontcache_reads_attempted << ','
           << metrics.dontcache_reads_succeeded << ','
           << metrics.dontcache_unsupported << ','
           << metrics.dontcache_fallbacks << ',' << metrics.buffered_reads << ','
           << metrics.bytes_dontcache << ',' << metrics.bytes_buffered
           << ",0,0,0,0\n";
}

void emit_cache_pollution(std::ostream& output, const Options& options) {
    for (const char* policy_name : {"baseline", "noreuse", "dontcache"}) {
        Options policy_options = options;
        policy_options.policy = policy_name;
        policy_options.workload = "sequential";
        policy_options.requests = options.hot_requests;
        TemporaryFile hot(options.hot_mib * 1024U * 1024U,
                           options.materialize_file);
        TemporaryFile scan(options.scan_mib * 1024U * 1024U,
                           options.materialize_file);
        policy_options.request_size = 1024U * 1024U;
        Options hot_options = policy_options;
        hot_options.policy = "baseline";
        hot_options.request_size = 4096U;
        const std::vector<Range> hot_ranges = make_ranges(
            hot_options,
            options.hot_requests,
            options.hot_mib * 1024U * 1024U);
        const std::vector<Range> scan_ranges = make_ranges(
            policy_options,
            options.scan_mib,
            options.scan_mib * 1024U * 1024U);
        RunResult warm = run_batch(hot_options, hot.fd(), hot_ranges);
        (void)warm;
        const Residency before_residency = residency(
            hot.fd(), options.hot_mib * 1024U * 1024U);
        const RunResult before = run_batch(hot_options, hot.fd(), hot_ranges);
        const RunResult scan_result = run_batch(policy_options, scan.fd(), scan_ranges);
        const Residency after_residency = residency(
            hot.fd(), options.hot_mib * 1024U * 1024U);
        const RunResult after = run_batch(hot_options, hot.fd(), hot_ranges);
        const double before_fraction = before_residency.pages == 0
                                           ? 0.0
                                           : static_cast<double>(before_residency.resident) /
                                                 static_cast<double>(before_residency.pages);
        const double after_fraction = after_residency.pages == 0
                                          ? 0.0
                                          : static_cast<double>(after_residency.resident) /
                                                static_cast<double>(after_residency.pages);
        const auto& metrics = scan_result.after;
        const double tasks = static_cast<double>(
            metrics.executor_tasks_completed - scan_result.before.executor_tasks_completed);
        const double seconds = scan_result.wall_ms / 1000.0;
        output << "cache-pollution," << policy_name << ",sequential,OK,"
               << kernel_name() << ',' << filesystem_name(".") << ','
               << (options.materialize_file ? "materialized" : "sparse")
               << ",warm-buffered," << options.workers << ',' << options.qd << ','
               << options.scan_mib << ',' << nominal_request_size(policy_options) << ','
               << std::fixed << std::setprecision(3)
               << scan_result.wall_ms << ','
               << (seconds == 0.0 ? 0.0 : tasks / seconds) << ','
               << percentile(before.completion_ms, 0.50) << ','
               << percentile(before.completion_ms, 0.95) << ','
               << percentile(before.completion_ms, 0.99) << ','
               << scan_result.usage.cpu_seconds << ','
               << scan_result.usage.minor_faults << ','
               << scan_result.usage.major_faults << ','
               << metrics.logical_reads - scan_result.before.logical_reads << ','
               << metrics.physical_reads - scan_result.before.physical_reads << ','
               << metrics.kernel_advice_sequential << ','
               << metrics.kernel_advice_random << ','
               << metrics.kernel_advice_noreuse << ','
               << metrics.kernel_advice_failures << ','
               << metrics.dontcache_reads_attempted << ','
               << metrics.dontcache_reads_succeeded << ','
               << metrics.dontcache_unsupported << ','
               << metrics.dontcache_fallbacks << ',' << metrics.buffered_reads << ','
               << metrics.bytes_dontcache << ',' << metrics.bytes_buffered << ','
               << percentile(before.completion_ms, 0.50) << ','
               << percentile(after.completion_ms, 0.50) << ',' << before_fraction << ','
               << after_fraction << '\n';
    }
}

} // namespace

int main(int argc, char** argv) {
    try {
        const Options options = parse_options(argc, argv);
        std::ostream* output = &std::cout;
        std::ofstream output_file;
        if (!options.output.empty()) {
            output_file.open(options.output);
            if (!output_file) {
                throw std::runtime_error("could not open output: " + options.output);
            }
            output = &output_file;
        }
        emit_header(*output);
        if (options.mode == "cache-pollution") {
            emit_cache_pollution(*output, options);
        } else {
            emit_scan(*output, options);
        }
    } catch (const std::exception& error) {
        std::cerr << "phase 9 benchmark failed: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
