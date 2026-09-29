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
#include <mutex>
#include <random>
#include <stdexcept>
#include <string>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/statfs.h>
#include <sys/utsname.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

struct Options {
    std::string mode{"scan"};
    std::string policy{"buffered"};
    std::string workload{"sequential"};
    std::size_t qd{16};
    std::size_t workers{16};
    std::size_t requests{64};
    std::size_t request_size{};
    std::size_t repetitions{1};
    std::size_t hot_mib{4};
    std::size_t hot_requests{1024};
    std::size_t scan_mib{64};
    std::size_t cache_pressure_mib{};
    std::size_t producers{1};
    std::size_t direct_threshold{};
    bool adaptive{false};
    bool materialize_file{false};
};

struct Range {
    std::uint64_t offset{};
    std::size_t length{};
};

struct Usage {
    double cpu_seconds{};
    long minor_faults{};
    long major_faults{};
    long voluntary_context_switches{};
    long involuntary_context_switches{};
};

struct Pending {
    intentio::Submission submission;
    Clock::time_point submitted_at;
};

struct RunResult {
    std::vector<double> completion_ms;
    double wall_ms{};
    Usage usage{};
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
        char path[] = "./intentio-phase10-XXXXXX";
        fd_ = ::mkstemp(path);
        if (fd_ < 0) {
            throw std::runtime_error(std::string("mkstemp: ") + std::strerror(errno));
        }
        path_ = path;
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
        if (!path_.empty()) {
            ::unlink(path_.c_str());
        }
    }

    TemporaryFile(const TemporaryFile&) = delete;
    TemporaryFile& operator=(const TemporaryFile&) = delete;

    [[nodiscard]] int fd() const noexcept { return fd_; }
    [[nodiscard]] const std::string& path() const noexcept { return path_; }

private:
    int fd_{-1};
    std::string path_;
};

struct BenchmarkSkip : std::runtime_error {
    explicit BenchmarkSkip(const std::string& message)
        : std::runtime_error(message) {}
};

void usage(const char* program) {
    std::cout << "usage: " << program << " [options]\n"
              << "  --mode scan|planner|duplicates|reread|cache-pressure|correctness\n"
              << "  --policy auto|buffered|dontcache|direct|threshold\n"
              << "  --workload sequential|random4k|random64k|mixed\n"
              << "  --qd N --workers N --requests N --request-size N\n"
              << "  --repetitions N --hot-mib N --hot-requests N --scan-mib N\n"
              << "  --cache-pressure-size N --producers N --direct-threshold N\n"
              << "  --adaptive --materialize-file\n";
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
        if (argument == "--adaptive") {
            options.adaptive = true;
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
        } else if (argument == "--request-size") {
            options.request_size = parse_size(value, "request-size");
        } else if (argument == "--repetitions") {
            options.repetitions = parse_size(value, "repetitions");
        } else if (argument == "--hot-mib") {
            options.hot_mib = parse_size(value, "hot-mib");
        } else if (argument == "--hot-requests") {
            options.hot_requests = parse_size(value, "hot-requests");
        } else if (argument == "--scan-mib") {
            options.scan_mib = parse_size(value, "scan-mib");
        } else if (argument == "--cache-pressure-size") {
            options.cache_pressure_mib = parse_size(value, "cache-pressure-size");
        } else if (argument == "--producers") {
            options.producers = parse_size(value, "producers");
        } else if (argument == "--direct-threshold") {
            options.direct_threshold = parse_size(value, "direct-threshold");
        } else {
            throw std::invalid_argument("unknown option: " + argument);
        }
    }
    if (options.mode != "scan" && options.mode != "planner" &&
        options.mode != "duplicates" && options.mode != "reread" &&
        options.mode != "cache-pressure" && options.mode != "correctness") {
        throw std::invalid_argument("unknown mode: " + options.mode);
    }
    if (options.policy != "auto" && options.policy != "buffered" &&
        options.policy != "dontcache" && options.policy != "direct" &&
        options.policy != "threshold") {
        throw std::invalid_argument("unknown policy: " + options.policy);
    }
    if (options.policy == "threshold" && options.direct_threshold == 0) {
        throw std::invalid_argument(
            "threshold policy requires --direct-threshold N");
    }
    if (options.workload != "sequential" && options.workload != "random4k" &&
        options.workload != "random64k" && options.workload != "mixed") {
        throw std::invalid_argument("unknown workload: " + options.workload);
    }
    return options;
}

Usage read_usage() {
    rusage usage{};
    if (::getrusage(RUSAGE_SELF, &usage) != 0) {
        throw std::runtime_error(std::string("getrusage: ") + std::strerror(errno));
    }
    const double user = static_cast<double>(usage.ru_utime.tv_sec) +
                        static_cast<double>(usage.ru_utime.tv_usec) / 1.0e6;
    const double system = static_cast<double>(usage.ru_stime.tv_sec) +
                          static_cast<double>(usage.ru_stime.tv_usec) / 1.0e6;
    return Usage{user + system,
                 usage.ru_minflt,
                 usage.ru_majflt,
                 usage.ru_nvcsw,
                 usage.ru_nivcsw};
}

double percentile(std::vector<double> values, double quantile) {
    if (values.empty()) {
        return 0.0;
    }
    std::sort(values.begin(), values.end());
    const std::size_t index = static_cast<std::size_t>(
        quantile * static_cast<double>(values.size() - 1U));
    return values[index];
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

std::vector<Range> make_ranges(const Options& options,
                               std::size_t request_count,
                               std::size_t file_size) {
    const std::size_t length = nominal_request_size(options);
    const std::uint64_t stride =
        options.workload == "sequential" ? static_cast<std::uint64_t>(length)
                                          : 2U * 1024U * 1024U;
    std::vector<Range> ranges;
    ranges.reserve(request_count);
    std::mt19937_64 generator(0xA10C0FFEEULL);
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
        if (offset + actual_length > file_size) {
            throw std::invalid_argument("benchmark file is too small for workload");
        }
        ranges.push_back({offset, actual_length});
    }
    if (options.workload == "random4k" || options.workload == "random64k" ||
        options.workload == "mixed") {
        std::shuffle(ranges.begin(), ranges.end(), generator);
    }
    return ranges;
}

std::size_t file_size_for(const Options& options, std::size_t request_count) {
    if (options.mode == "planner") {
        return request_count * (65536U + 4096U) + 1024U * 1024U;
    }
    if (options.mode == "duplicates") {
        return 1024U * 1024U;
    }
    const std::size_t length = nominal_request_size(options);
    const std::uint64_t stride =
        options.workload == "sequential" ? static_cast<std::uint64_t>(length)
                                          : 2U * 1024U * 1024U;
    std::uint64_t end = 0;
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
        end = std::max(end, offset + static_cast<std::uint64_t>(actual_length));
    }
    return static_cast<std::size_t>(end + 1024U * 1024U);
}

std::vector<Range> planner_ranges(std::size_t count) {
    std::vector<Range> ranges;
    ranges.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        ranges.push_back({
            static_cast<std::uint64_t>(index) * (65536U + 4096U),
            65536U,
        });
    }
    return ranges;
}

std::vector<Range> duplicate_ranges(std::size_t count) {
    return std::vector<Range>(count, Range{0, 4096});
}

void apply_group_policy(const Options& options, intentio::Group& group) {
    if (options.workload == "sequential") {
        group.access_pattern(intentio::AccessPattern::Sequential);
    } else if (options.workload == "random4k" ||
               options.workload == "random64k") {
        group.access_pattern(intentio::AccessPattern::Random);
    }

    if (options.policy == "auto") {
        group.reuse_hint(intentio::ReuseHint::ScanOnce);
        group.io_cache_mode(intentio::IOCacheMode::Auto);
    } else if (options.policy == "buffered") {
        group.io_cache_mode(intentio::IOCacheMode::Buffered);
    } else if (options.policy == "dontcache") {
        group.io_cache_mode(intentio::IOCacheMode::DontCache);
    } else if (options.policy == "direct") {
        group.io_cache_mode(intentio::IOCacheMode::Direct);
    } else if (options.policy == "threshold") {
        group.reuse_hint(intentio::ReuseHint::ScanOnce);
        group.io_cache_mode(intentio::IOCacheMode::Auto);
    }
}

RunResult run_batch(const Options& options,
                    const std::string& path,
                    const std::vector<Range>& ranges,
                    bool planner_enabled,
                    bool one_group = false) {
    intentio::File file = intentio::File::open(path);
    if (options.policy == "direct" && !file.capabilities().direct_io_supported) {
        throw BenchmarkSkip("managed file has no usable direct-I/O capability");
    }

    intentio::RuntimeOptions runtime_options;
    runtime_options.backend = intentio::Backend::ThreadedPread;
    runtime_options.threaded_worker_count = options.workers;
    runtime_options.execution.max_in_flight = options.qd;
    runtime_options.adaptive_execution.enabled = options.adaptive;
    runtime_options.kernel_io.enable_fadvise = false;
    runtime_options.kernel_io.auto_direct_threshold_bytes =
        options.policy == "threshold" ? options.direct_threshold : 0;
    runtime_options.coalescing.enabled = planner_enabled;
    runtime_options.deduplication.enabled = planner_enabled;
    intentio::Runtime runtime(runtime_options);

    RunResult result;
    result.before = runtime.metrics();
    result.completion_ms.reserve(ranges.size());
    std::vector<Pending> pending;
    pending.reserve(ranges.size());
    const Usage usage_start = read_usage();
    const auto wall_start = Clock::now();
    if (one_group) {
        auto group = runtime.create_group();
        for (const Range& range : ranges) {
            group.read(file, range.offset, range.length);
        }
        apply_group_policy(options, group);
        pending.push_back(Pending{runtime.submit(group), Clock::now()});
    } else if (options.producers == 1U) {
        for (const Range& range : ranges) {
            auto group = runtime.create_group();
            group.read(file, range.offset, range.length);
            apply_group_policy(options, group);
            pending.push_back(Pending{runtime.submit(group), Clock::now()});
        }
    } else {
        std::mutex pending_mutex;
        std::vector<std::thread> producers;
        producers.reserve(options.producers);
        for (std::size_t producer = 0; producer < options.producers; ++producer) {
            producers.emplace_back([&, producer] {
                for (std::size_t index = producer; index < ranges.size();
                     index += options.producers) {
                    auto group = runtime.create_group();
                    const Range& range = ranges[index];
                    group.read(file, range.offset, range.length);
                    apply_group_policy(options, group);
                    Pending item{runtime.submit(group), Clock::now()};
                    std::lock_guard lock(pending_mutex);
                    pending.push_back(std::move(item));
                }
            });
        }
        for (std::thread& producer : producers) {
            producer.join();
        }
    }

    for (Pending& item : pending) {
        const auto values = item.submission.get();
        const auto failed = std::find_if(
            values.begin(), values.end(), [](const intentio::ReadResult& value) {
                return !value.ok();
            });
        if (values.empty() || failed != values.end()) {
            const std::string reason = values.empty()
                                           ? "empty result"
                                           : failed->error.message();
            throw BenchmarkSkip("managed " + options.policy +
                                " read failed: " + reason);
        }
        result.completion_ms.push_back(
            std::chrono::duration<double, std::milli>(Clock::now() -
                                                       item.submitted_at)
                .count());
    }
    const Usage usage_stop = read_usage();
    result.wall_ms = std::chrono::duration<double, std::milli>(
                         Clock::now() - wall_start)
                         .count();
    result.usage = Usage{
        usage_stop.cpu_seconds - usage_start.cpu_seconds,
        usage_stop.minor_faults - usage_start.minor_faults,
        usage_stop.major_faults - usage_start.major_faults,
        usage_stop.voluntary_context_switches -
            usage_start.voluntary_context_switches,
        usage_stop.involuntary_context_switches -
            usage_start.involuntary_context_switches,
    };
    result.after = runtime.metrics();
    return result;
}

Residency residency(const std::string& path, std::size_t bytes) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return {};
    }
    const long page_size = ::sysconf(_SC_PAGESIZE);
    if (page_size <= 0 || bytes == 0) {
        ::close(fd);
        return {};
    }
    void* mapping = ::mmap(nullptr, bytes, PROT_READ, MAP_SHARED, fd, 0);
    ::close(fd);
    if (mapping == MAP_FAILED) {
        return {};
    }
    const std::size_t pages =
        (bytes + static_cast<std::size_t>(page_size) - 1U) /
        static_cast<std::size_t>(page_size);
    std::vector<unsigned char> state(pages);
    Residency result{0, pages};
    if (::mincore(mapping, bytes, state.data()) == 0) {
        result.resident = static_cast<std::size_t>(std::count_if(
            state.begin(), state.end(), [](unsigned char value) {
                return (value & 1U) != 0U;
            }));
    }
    ::munmap(mapping, bytes);
    return result;
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
    std::ostringstream value;
    value << "0x" << std::hex << static_cast<unsigned long long>(info.f_type);
    return value.str();
}

std::uint64_t delta(std::uint64_t after, std::uint64_t before) {
    return after >= before ? after - before : 0;
}

void emit_header(std::ostream& output) {
    output << "mode,policy,workload,status,kernel,filesystem,file_state,managed,"
              "direct_supported,workers,qd,requests,request_size,wall_ms,tasks_per_sec,"
              "p50_ms,p95_ms,p99_ms,cpu_seconds,minor_faults,major_faults,"
              "voluntary_context_switches,involuntary_context_switches,logical_reads,"
              "physical_reads,managed_file_reads,direct_attempted,direct_succeeded,direct_bytes,"
              "direct_unsupported,direct_alignment_fallbacks,direct_open_failures,"
              "dontcache_attempted,dontcache_succeeded,dontcache_unsupported,dontcache_fallbacks,"
              "buffered_reads,bytes_dontcache,bytes_buffered,auto_buffered,auto_dontcache,"
              "auto_direct,controller_windows,concurrency_increases,"
              "concurrency_decreases,current_concurrency,peak_concurrency,"
              "first_p50_ms,second_p50_ms,hot_residency_before,hot_residency_after,"
              "scan_residency_after\n";
}

void emit_run(std::ostream& output,
              const Options& options,
              const RunResult& result,
              const intentio::FileCapabilities& capabilities,
              const std::string& file_state,
              const char* mode,
              const std::string& workload,
              double first_p50 = 0.0,
              double second_p50 = 0.0,
              double hot_before = 0.0,
              double hot_after = 0.0,
              double scan_after = 0.0) {
    const auto& before = result.before;
    const auto& metrics = result.after;
    const double seconds = result.wall_ms / 1000.0;
    const double tasks = static_cast<double>(delta(
        metrics.executor_tasks_completed, before.executor_tasks_completed));
    output << mode << ',' << options.policy << ',' << workload << ",OK,"
           << kernel_name() << ',' << filesystem_name(".") << ',' << file_state
           << ",yes," << (capabilities.direct_io_supported ? "yes" : "no")
           << ',' << options.workers << ',' << options.qd << ',' << options.requests
           << ',' << nominal_request_size(options) << ',' << std::fixed
           << std::setprecision(3) << result.wall_ms << ','
           << (seconds == 0.0 ? 0.0 : tasks / seconds) << ','
           << percentile(result.completion_ms, 0.50) << ','
           << percentile(result.completion_ms, 0.95) << ','
           << percentile(result.completion_ms, 0.99) << ','
           << result.usage.cpu_seconds << ',' << result.usage.minor_faults << ','
           << result.usage.major_faults << ','
           << result.usage.voluntary_context_switches << ','
           << result.usage.involuntary_context_switches << ','
           << delta(metrics.logical_reads, before.logical_reads) << ','
           << delta(metrics.physical_reads, before.physical_reads) << ','
           << delta(metrics.managed_file_reads, before.managed_file_reads) << ','
           << delta(metrics.direct_reads_attempted, before.direct_reads_attempted)
           << ',' << delta(metrics.direct_reads_succeeded,
                              before.direct_reads_succeeded)
           << ',' << delta(metrics.direct_bytes, before.direct_bytes) << ','
           << delta(metrics.direct_unsupported, before.direct_unsupported) << ','
           << delta(metrics.direct_alignment_fallbacks,
                    before.direct_alignment_fallbacks)
           << ',' << delta(metrics.direct_open_failures, before.direct_open_failures)
           << ',' << delta(metrics.dontcache_reads_attempted,
                              before.dontcache_reads_attempted)
           << ',' << delta(metrics.dontcache_reads_succeeded,
                              before.dontcache_reads_succeeded)
           << ',' << delta(metrics.dontcache_unsupported,
                              before.dontcache_unsupported)
           << ',' << delta(metrics.dontcache_fallbacks, before.dontcache_fallbacks)
           << ',' << delta(metrics.buffered_reads, before.buffered_reads) << ','
           << delta(metrics.bytes_dontcache, before.bytes_dontcache) << ','
           << delta(metrics.bytes_buffered, before.bytes_buffered) << ','
           << delta(metrics.auto_selected_buffered, before.auto_selected_buffered)
           << ',' << delta(metrics.auto_selected_dontcache,
                              before.auto_selected_dontcache)
           << ',' << delta(metrics.auto_selected_direct, before.auto_selected_direct)
           << ',' << delta(metrics.controller_windows, before.controller_windows)
           << ',' << delta(metrics.concurrency_increases,
                              before.concurrency_increases)
           << ',' << delta(metrics.concurrency_decreases,
                              before.concurrency_decreases)
           << ',' << metrics.current_concurrency << ',' << metrics.peak_concurrency
           << ',' << first_p50 << ',' << second_p50 << ',' << hot_before << ','
           << hot_after << ',' << scan_after << '\n';
}

void emit_skip(std::ostream& output,
               const Options& options,
               const char* mode,
               const std::string& workload,
               const std::string& reason) {
    output << mode << ',' << options.policy << ',' << workload << ",SKIP,"
           << kernel_name() << ',' << filesystem_name(".") << ','
           << (options.materialize_file ? "materialized" : "sparse") << ",yes,"
           << "no," << options.workers << ',' << options.qd << ',' << options.requests
           << ',' << nominal_request_size(options);
    for (int index = 0; index < 39; ++index) {
        output << ",0";
    }
    output << '\n';
    std::cerr << "phase10 skip: " << reason << '\n';
}

void emit_scan(std::ostream& output, const Options& options) {
    const std::size_t file_size = file_size_for(options, options.requests);
    TemporaryFile file(file_size, options.materialize_file);
    const auto ranges = make_ranges(options, options.requests, file_size);
    try {
        RunResult last;
        for (std::size_t repetition = 0; repetition < options.repetitions; ++repetition) {
            last = run_batch(options, file.path(), ranges, false);
        }
        intentio::File managed = intentio::File::open(file.path());
        const std::string reported_workload =
            options.policy == "threshold"
                ? options.workload + "-threshold-" +
                      std::to_string(options.direct_threshold)
                : options.workload;
        emit_run(output,
                 options,
                 last,
                 managed.capabilities(),
                 options.materialize_file ? "materialized" : "sparse",
                 "scan",
                 reported_workload);
    } catch (const BenchmarkSkip& skip) {
        emit_skip(output, options, "scan", options.workload, skip.what());
    }
}

void emit_planner(std::ostream& output, Options options) {
    options.requests = std::max(options.requests, std::size_t{128});
    options.workload = "sequential";
    const std::size_t file_size = file_size_for(options, options.requests);
    TemporaryFile file(file_size, options.materialize_file);
    const auto ranges = planner_ranges(options.requests);
    try {
        RunResult result = run_batch(options, file.path(), ranges, true, true);
        intentio::File managed = intentio::File::open(file.path());
        emit_run(output,
                 options,
                 result,
                 managed.capabilities(),
                 options.materialize_file ? "materialized" : "sparse",
                 "planner",
                 "merged64k");
    } catch (const BenchmarkSkip& skip) {
        emit_skip(output, options, "planner", "merged64k", skip.what());
    }
}

void emit_duplicates(std::ostream& output, Options options) {
    options.requests = std::max(options.requests, std::size_t{1024});
    options.workload = "random4k";
    TemporaryFile file(file_size_for(options, options.requests), options.materialize_file);
    const auto ranges = duplicate_ranges(options.requests);
    try {
        RunResult result = run_batch(options, file.path(), ranges, true, true);
        intentio::File managed = intentio::File::open(file.path());
        emit_run(output,
                 options,
                 result,
                 managed.capabilities(),
                 options.materialize_file ? "materialized" : "sparse",
                 "duplicates",
                 "duplicate4k");
    } catch (const BenchmarkSkip& skip) {
        emit_skip(output, options, "duplicates", "duplicate4k", skip.what());
    }
}

void emit_reread(std::ostream& output, Options options) {
    const std::size_t file_size = file_size_for(options, options.requests);
    TemporaryFile file(file_size, options.materialize_file);
    const auto ranges = make_ranges(options, options.requests, file_size);
    try {
        const RunResult first = run_batch(options, file.path(), ranges, false);
        const RunResult second = run_batch(options, file.path(), ranges, false);
        intentio::File managed = intentio::File::open(file.path());
        emit_run(output,
                 options,
                 second,
                 managed.capabilities(),
                 options.materialize_file ? "materialized" : "sparse",
                 "reread",
                 options.workload,
                 percentile(first.completion_ms, 0.50),
                 percentile(second.completion_ms, 0.50));
    } catch (const BenchmarkSkip& skip) {
        emit_skip(output, options, "reread", options.workload, skip.what());
    }
}

void emit_cache_pressure(std::ostream& output, Options options) {
    const std::size_t scan_mib = options.cache_pressure_mib != 0
                                     ? options.cache_pressure_mib
                                     : options.scan_mib;
    options.workload = "sequential";
    options.request_size = 1024U * 1024U;
    options.requests = scan_mib;
    TemporaryFile hot(options.hot_mib * 1024U * 1024U, options.materialize_file);
    TemporaryFile scan(scan_mib * 1024U * 1024U, options.materialize_file);
    Options hot_options = options;
    hot_options.policy = "buffered";
    hot_options.request_size = 4096U;
    hot_options.requests = options.hot_requests;
    const auto hot_ranges = make_ranges(
        hot_options, options.hot_requests, options.hot_mib * 1024U * 1024U);
    const auto scan_ranges = make_ranges(
        options, options.requests, scan_mib * 1024U * 1024U);

    try {
        (void)run_batch(hot_options, hot.path(), hot_ranges, false);
        const Residency before = residency(
            hot.path(), options.hot_mib * 1024U * 1024U);
        const RunResult hot_before = run_batch(
            hot_options, hot.path(), hot_ranges, false);
        const RunResult scan_result = run_batch(
            options, scan.path(), scan_ranges, false);
        const Residency scan_residency = residency(
            scan.path(), scan_mib * 1024U * 1024U);
        const Residency after = residency(
            hot.path(), options.hot_mib * 1024U * 1024U);
        const RunResult hot_after = run_batch(
            hot_options, hot.path(), hot_ranges, false);
        intentio::File managed = intentio::File::open(scan.path());
        const double before_fraction = before.pages == 0
                                           ? 0.0
                                           : static_cast<double>(before.resident) /
                                                 static_cast<double>(before.pages);
        const double after_fraction = after.pages == 0
                                          ? 0.0
                                          : static_cast<double>(after.resident) /
                                                static_cast<double>(after.pages);
        emit_run(output,
                 options,
                 scan_result,
                 managed.capabilities(),
                 options.materialize_file ? "materialized" : "sparse",
                 "cache-pressure",
                 "hot-scan",
                 percentile(hot_before.completion_ms, 0.50),
                 percentile(hot_after.completion_ms, 0.50),
                 before_fraction,
                 after_fraction,
                 scan_residency.pages == 0
                     ? 0.0
                     : static_cast<double>(scan_residency.resident) /
                           static_cast<double>(scan_residency.pages));
    } catch (const BenchmarkSkip& skip) {
        emit_skip(output, options, "cache-pressure", "hot-scan", skip.what());
    }
}

void emit_correctness(std::ostream& output, Options options) {
    options.requests = 4;
    options.request_size = 4096;
    options.workload = "sequential";
    const std::size_t file_size = 16384;
    TemporaryFile file(file_size, true);
    const std::vector<Range> ranges{{0, 4096}, {4096, 4096}, {8192, 4096}, {12288, 4096}};
    try {
        RunResult result = run_batch(options, file.path(), ranges, false);
        intentio::File managed = intentio::File::open(file.path());
        emit_run(output,
                 options,
                 result,
                 managed.capabilities(),
                 "materialized",
                 "correctness",
                 "aligned4k");
    } catch (const BenchmarkSkip& skip) {
        emit_skip(output, options, "correctness", "aligned4k", skip.what());
    }
}

} // namespace

int main(int argc, char** argv) {
    try {
        const Options options = parse_options(argc, argv);
        emit_header(std::cout);
        if (options.mode == "planner") {
            emit_planner(std::cout, options);
        } else if (options.mode == "duplicates") {
            emit_duplicates(std::cout, options);
        } else if (options.mode == "reread") {
            emit_reread(std::cout, options);
        } else if (options.mode == "cache-pressure") {
            emit_cache_pressure(std::cout, options);
        } else if (options.mode == "correctness") {
            emit_correctness(std::cout, options);
        } else {
            emit_scan(std::cout, options);
        }
    } catch (const std::exception& error) {
        std::cerr << "phase 10 benchmark failed: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
