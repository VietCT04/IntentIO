#include <intentio/intentio.hpp>

#include <algorithm>
#include <barrier>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <mutex>
#include <numeric>
#include <random>
#include <stdexcept>
#include <string>
#include <sys/resource.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

struct Options {
    std::string mode{"executor"};
    std::string workload{"random4k"};
    std::string backend{"threaded"};
    std::string output{};
    std::size_t qd{8};
    std::size_t max_qd{8};
    std::size_t batch_size{1};
    std::size_t workers{8};
    std::size_t producers{1};
    std::size_t groups{64};
    std::size_t requests{64};
    bool materialize_file{false};
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

struct PendingSubmission {
    intentio::Submission submission;
    Clock::time_point submitted_at;
};

struct TimePoint {
    double time_ms{};
    std::size_t current_qd{};
    double ewma_latency_ms{};
    double completion_rate{};
    std::uint64_t queue_depth{};
};

struct RunResult {
    std::vector<double> submit_us;
    std::vector<double> completion_ms;
    std::vector<TimePoint> time_series;
    double wall_ms{};
    ResourceUsage usage{};
    intentio::MetricsSnapshot before{};
    intentio::MetricsSnapshot after{};
};

class TemporaryFile {
public:
    TemporaryFile(std::size_t size, bool materialize) {
        char path[] = "/tmp/intentio-phase7-XXXXXX";
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
            std::vector<std::byte> chunk(chunk_size);
            for (std::size_t index = 0; index < chunk.size(); ++index) {
                chunk[index] = static_cast<std::byte>(index % 251U);
            }
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
              << "  --mode executor|fixed|adaptive|changing|fairness\n"
              << "  --workload random4k|random64k|sequential|mixed\n"
              << "  --backend threaded|auto\n"
              << "  --qd N --max-qd N --executor-batch-size N --workers N --producers N\n"
              << "  --groups N --requests N --materialize-file --output FILE\n";
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
        if (index + 1 >= argc && argument != "--materialize-file") {
            throw std::invalid_argument("missing value for " + argument);
        }
        if (argument == "--materialize-file") {
            options.materialize_file = true;
            continue;
        }
        const std::string value = argv[++index];
        if (argument == "--mode") {
            options.mode = value;
        } else if (argument == "--workload") {
            options.workload = value;
        } else if (argument == "--backend") {
            options.backend = value;
        } else if (argument == "--qd") {
            options.qd = parse_size(value, "qd");
        } else if (argument == "--max-qd") {
            options.max_qd = parse_size(value, "max-qd");
        } else if (argument == "--executor-batch-size") {
            options.batch_size = parse_size(value, "executor-batch-size");
        } else if (argument == "--workers") {
            options.workers = parse_size(value, "workers");
        } else if (argument == "--producers") {
            options.producers = parse_size(value, "producers");
        } else if (argument == "--groups") {
            options.groups = parse_size(value, "groups");
        } else if (argument == "--requests") {
            options.requests = parse_size(value, "requests");
        } else if (argument == "--output") {
            options.output = value;
        } else {
            throw std::invalid_argument("unknown option: " + argument);
        }
    }

    if (options.mode != "executor" && options.mode != "fixed" &&
        options.mode != "adaptive" && options.mode != "changing" &&
        options.mode != "fairness") {
        throw std::invalid_argument("unknown mode: " + options.mode);
    }
    if (options.workload != "random4k" && options.workload != "random64k" &&
        options.workload != "sequential" && options.workload != "mixed") {
        throw std::invalid_argument("unknown workload: " + options.workload);
    }
    if (options.backend != "threaded" && options.backend != "auto") {
        throw std::invalid_argument("backend must be threaded or auto");
    }
    if (options.max_qd < options.qd) {
        throw std::invalid_argument("max-qd must be at least qd");
    }
    return options;
}

intentio::Backend parse_backend(const std::string& backend) {
    return backend == "auto" ? intentio::Backend::Auto
                              : intentio::Backend::ThreadedPread;
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
    if (values.empty()) {
        return 0.0;
    }
    std::sort(values.begin(), values.end());
    const std::size_t index = static_cast<std::size_t>(
        quantile * static_cast<double>(values.size() - 1));
    return values[index];
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
        const std::size_t length = workload == "random4k" ? four_kib : sixty_four_kib;
        std::vector<std::size_t> order(request_count);
        std::iota(order.begin(), order.end(), 0U);
        std::mt19937_64 generator(0x7A17C0DEULL);
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
    } else {
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
        end = std::max(end,
                       range.offset + static_cast<std::uint64_t>(range.length));
    }
    return static_cast<std::size_t>(end + 1024U * 1024U);
}

void add_ranges(intentio::Group& group,
                const std::vector<Range>& ranges,
                int fd) {
    for (const Range& range : ranges) {
        group.read(fd, range.offset, range.length);
    }
}

intentio::RuntimeOptions runtime_options(const Options& options,
                                         bool adaptive) {
    intentio::RuntimeOptions result;
    result.backend = parse_backend(options.backend);
    result.threaded_worker_count = options.workers;
    result.executor_batch_size = options.batch_size;
    result.execution.max_in_flight = adaptive ? options.max_qd : options.qd;
    result.adaptive_execution.enabled = adaptive;
    result.coalescing.enabled = false;
    return result;
}

RunResult run_concurrent_batch(intentio::Runtime& runtime,
                               const Options& options,
                               const std::vector<Range>& ranges,
                               int fd,
                               std::size_t groups,
                               bool expect_errors,
                               const std::string& phase) {
    (void)phase;
    RunResult result;
    result.before = runtime.metrics();
    result.submit_us.reserve(groups);
    result.completion_ms.reserve(groups);
    result.time_series.reserve(groups);
    const ResourceUsage usage_start = read_usage();
    const auto wall_start = Clock::now();
    std::mutex samples_mutex;
    std::barrier start_barrier(static_cast<std::ptrdiff_t>(options.producers + 1U));
    std::vector<std::thread> producers;
    producers.reserve(options.producers);

    for (std::size_t producer = 0; producer < options.producers; ++producer) {
        producers.emplace_back([&, producer] {
            const std::size_t first = groups * producer / options.producers;
            const std::size_t last = groups * (producer + 1U) / options.producers;
            std::vector<PendingSubmission> pending;
            pending.reserve(last - first);
            start_barrier.arrive_and_wait();
            for (std::size_t index = first; index < last; ++index) {
                auto group = runtime.create_group();
                add_ranges(group, ranges, fd);
                if (expect_errors) {
                    group.read(-1, 0, 1);
                }
                const auto submit_start = Clock::now();
                auto submission = runtime.submit(group);
                const auto submit_stop = Clock::now();
                const double submit_us = std::chrono::duration<double, std::micro>(
                                               submit_stop - submit_start)
                                               .count();
                pending.push_back(
                    PendingSubmission{std::move(submission), submit_start});
                std::lock_guard lock(samples_mutex);
                result.submit_us.push_back(submit_us);
            }
            for (PendingSubmission& item : pending) {
                item.submission.wait();
                const auto completed_at = Clock::now();
                const double completion_ms =
                    std::chrono::duration<double, std::milli>(
                        completed_at - item.submitted_at)
                        .count();
                const auto metrics = runtime.metrics();
                std::lock_guard lock(samples_mutex);
                result.completion_ms.push_back(completion_ms);
                result.time_series.push_back(TimePoint{
                    std::chrono::duration<double, std::milli>(
                        completed_at - wall_start)
                        .count(),
                    metrics.current_concurrency,
                    metrics.ewma_completion_latency_ms,
                    metrics.completion_rate,
                    metrics.executor_queue_depth,
                });
            }
        });
    }
    start_barrier.arrive_and_wait();
    for (std::thread& producer : producers) {
        producer.join();
    }
    const auto wall_stop = Clock::now();
    const ResourceUsage usage_stop = read_usage();
    result.wall_ms = std::chrono::duration<double, std::milli>(
                         wall_stop - wall_start)
                         .count();
    result.usage = ResourceUsage{
        usage_stop.cpu_seconds - usage_start.cpu_seconds,
        usage_stop.voluntary_context_switches - usage_start.voluntary_context_switches,
        usage_stop.involuntary_context_switches - usage_start.involuntary_context_switches,
    };
    result.after = runtime.metrics();
    return result;
}

void emit_header(std::ostream& output) {
    output << "mode,workload,phase,requested_backend,actual_backend,workers,max_qd,"
              "executor_batch_size,"
              "producers,groups,requests,file_state,cache_state,wall_ms,tasks_per_sec,"
              "submit_p50_us,submit_p95_us,completion_p50_ms,completion_p95_ms,"
              "completion_p99_ms,cpu_seconds,voluntary_context_switches,"
              "involuntary_context_switches,logical_reads,physical_reads,current_qd,"
              "peak_qd,controller_windows,ewma_latency_ms,completion_rate,"
              "executor_tasks_submitted,executor_tasks_completed,executor_queue_depth,"
              "queue_wait_ns,execution_ns,completion_dispatch_ns,worker_wakeups,"
              "spurious_wakeups,workers_busy,workers_idle,max_queue_depth,"
              "executor_peak_in_flight,time_series\n";
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

std::string join_series(const std::vector<TimePoint>& values) {
    std::string result;
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (index != 0) {
            result += ';';
        }
        const TimePoint& point = values[index];
        result += std::to_string(point.time_ms) + "|" +
                  std::to_string(point.current_qd) + "|" +
                  std::to_string(point.ewma_latency_ms) + "|" +
                  std::to_string(point.completion_rate) + "|" +
                  std::to_string(point.queue_depth);
    }
    return result;
}

void emit_result(std::ostream& output,
                 const std::string& mode,
                 const std::string& workload,
                 const std::string& phase,
                 const Options& options,
                 const std::string& file_state,
                 const RunResult& result,
                 intentio::Backend actual_backend,
                 std::size_t groups,
                 std::size_t requests) {
    const auto& metrics = result.after;
    const double seconds = result.wall_ms / 1000.0;
    const double tasks = static_cast<double>(
        metrics.executor_tasks_completed - result.before.executor_tasks_completed);
    const double tasks_per_sec = seconds == 0.0 ? 0.0 : tasks / seconds;
    output << mode << ',' << workload << ',' << phase << ',' << options.backend << ','
           << backend_name(actual_backend) << ',' << options.workers << ','
           << (mode == "adaptive" ? options.max_qd : options.qd) << ','
           << options.batch_size << ','
           << options.producers << ',' << groups << ',' << requests << ','
           << file_state << ",warm-buffered," << std::fixed << std::setprecision(3)
           << result.wall_ms << ',' << tasks_per_sec << ','
           << percentile(result.submit_us, 0.50) << ','
           << percentile(result.submit_us, 0.95) << ','
           << percentile(result.completion_ms, 0.50) << ','
           << percentile(result.completion_ms, 0.95) << ','
           << percentile(result.completion_ms, 0.99) << ','
           << result.usage.cpu_seconds << ','
           << result.usage.voluntary_context_switches << ','
           << result.usage.involuntary_context_switches << ','
           << metrics.logical_reads - result.before.logical_reads << ','
           << metrics.physical_reads - result.before.physical_reads << ','
           << metrics.current_concurrency << ',' << metrics.peak_concurrency << ','
           << metrics.controller_windows << ',' << metrics.ewma_completion_latency_ms << ','
           << metrics.completion_rate << ',' << metrics.executor_tasks_submitted << ','
           << metrics.executor_tasks_completed << ',' << metrics.executor_queue_depth << ','
           << metrics.executor_queue_wait_ns << ',' << metrics.executor_execution_ns << ','
           << metrics.executor_completion_dispatch_ns << ','
           << metrics.executor_worker_wakeups << ','
           << metrics.executor_spurious_wakeups << ',' << metrics.executor_workers_busy << ','
           << metrics.executor_workers_idle << ',' << metrics.executor_max_queue_depth << ','
           << metrics.executor_peak_in_flight << ',' << join_series(result.time_series)
           << '\n';
}

void run_executor(std::ostream& output, const Options& options) {
    intentio::Runtime runtime(runtime_options(options, false));
    std::vector<Range> tiny;
    tiny.reserve(options.requests);
    for (std::size_t index = 0; index < options.requests; ++index) {
        tiny.push_back({static_cast<std::uint64_t>(index), 1});
    }
    const RunResult result = run_concurrent_batch(
        runtime, options, tiny, -1, options.groups, false, "executor");
    emit_result(output,
                "executor",
                "synthetic-invalid-fd",
                "executor",
                options,
                "none",
                result,
                runtime.backend(),
                options.groups,
                options.requests);
}

void run_workload(std::ostream& output,
                  const Options& options,
                  const std::string& mode,
                  const std::string& workload,
                  intentio::Runtime& runtime,
                  TemporaryFile& file) {
    const auto ranges = make_ranges(workload, options.requests);
    const RunResult result = run_concurrent_batch(
        runtime, options, ranges, file.fd(), options.groups, false, workload);
    emit_result(output,
                mode,
                workload,
                workload,
                options,
                options.materialize_file ? "materialized" : "sparse",
                result,
                runtime.backend(),
                options.groups,
                options.requests);
}

void run_changing(std::ostream& output, const Options& options) {
    std::vector<Range> all_ranges;
    for (const char* workload : {"random4k", "sequential", "random64k", "mixed"}) {
        const auto ranges = make_ranges(workload, options.requests);
        all_ranges.insert(all_ranges.end(), ranges.begin(), ranges.end());
    }
    TemporaryFile file(required_file_size(all_ranges), options.materialize_file);
    intentio::Runtime runtime(runtime_options(options, true));
    for (const char* workload : {"random4k", "sequential", "random64k", "mixed"}) {
        run_workload(output, options, "adaptive", workload, runtime, file);
    }
}

struct FairnessResult {
    double small_p50_ms{};
    double small_p95_ms{};
    std::size_t small_before_large_done{};
    std::size_t small_count{};
};

FairnessResult run_fairness_case(const Options& options,
                                 TemporaryFile& file,
                                 bool latency_priority) {
    intentio::Runtime runtime(runtime_options(options, false));
    auto large_group = runtime.create_group();
    const auto large_ranges = make_ranges("random4k", options.requests * 64U);
    add_ranges(large_group, large_ranges, file.fd());
    large_group.latency_class(intentio::LatencyClass::Throughput);
    auto large = runtime.submit(large_group);

    std::vector<double> small_latencies;
    std::size_t small_before_large_done = 0;
    for (std::size_t index = 0; index < options.groups; ++index) {
        auto small_group = runtime.create_group();
        if (latency_priority) {
            small_group.latency_class(intentio::LatencyClass::LatencySensitive);
        }
        const auto start = Clock::now();
        small_group.read(file.fd(), static_cast<std::uint64_t>(index * 4096U), 4096);
        auto small = runtime.submit(small_group);
        small.wait();
        const auto stop = Clock::now();
        small_latencies.push_back(
            std::chrono::duration<double, std::milli>(stop - start).count());
        if (!large.ready()) {
            ++small_before_large_done;
        }
    }
    large.wait();
    return FairnessResult{
        percentile(small_latencies, 0.50),
        percentile(small_latencies, 0.95),
        small_before_large_done,
        small_latencies.size(),
    };
}

void run_fairness(std::ostream& output, const Options& options) {
    const auto ranges = make_ranges("random4k", options.requests * 64U);
    TemporaryFile file(required_file_size(ranges), options.materialize_file);
    const FairnessResult fair = run_fairness_case(options, file, true);
    const FairnessResult normal = run_fairness_case(options, file, false);
    output << "mode,workload,case,requested_backend,actual_backend,workers,qd,"
              "executor_batch_size,"
              "producers,groups,requests,file_state,cache_state,fair_p50_ms,"
              "fair_p95_ms,default_p50_ms,default_p95_ms,fair_small_before_large_done,"
              "default_small_before_large_done,fair_small_count,default_small_count\n";
    output << "fairness,random4k,latency-sensitive-vs-default," << options.backend
           << ",threaded," << options.workers << ',' << options.qd << ','
           << options.batch_size << ','
           << options.producers << ',' << options.groups << ',' << options.requests
           << ',' << (options.materialize_file ? "materialized" : "sparse")
           << ",warm-buffered," << std::fixed << std::setprecision(3) << fair.small_p50_ms
           << ',' << fair.small_p95_ms << ',' << normal.small_p50_ms << ','
           << normal.small_p95_ms << ',' << fair.small_before_large_done << ','
           << normal.small_before_large_done << ',' << fair.small_count << ','
           << normal.small_count << '\n';
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

        if (options.mode == "fairness") {
            run_fairness(*output, options);
            return 0;
        }

        emit_header(*output);
        if (options.mode == "executor") {
            run_executor(*output, options);
            return 0;
        }
        if (options.mode == "changing") {
            run_changing(*output, options);
            return 0;
        }

        const auto ranges = make_ranges(options.workload, options.requests);
        TemporaryFile file(required_file_size(ranges), options.materialize_file);
        const bool adaptive = options.mode == "adaptive";
        intentio::Runtime runtime(runtime_options(options, adaptive));
        run_workload(*output, options, options.mode, options.workload, runtime, file);
    } catch (const std::exception& error) {
        std::cerr << "phase 7 benchmark failed: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
