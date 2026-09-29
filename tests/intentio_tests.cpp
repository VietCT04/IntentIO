#include <intentio/intentio.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <vector>

namespace {

class TestFile {
public:
    explicit TestFile(std::size_t size) {
        size_ = size;
        char path[] = "/tmp/intentio-test-XXXXXX";
        fd_ = ::mkstemp(path);
        if (fd_ < 0) {
            throw std::runtime_error(std::string("mkstemp: ") + std::strerror(errno));
        }
        ::unlink(path);

        if (::ftruncate(fd_, static_cast<off_t>(size)) != 0) {
            throw std::runtime_error(std::string("ftruncate: ") + std::strerror(errno));
        }

        std::vector<std::byte> contents(size);
        for (std::size_t index = 0; index < size; ++index) {
            contents[index] = static_cast<std::byte>(index % 251U);
        }
        std::size_t written = 0;
        while (written < contents.size()) {
            const ssize_t result = ::pwrite(fd_,
                                            contents.data() + written,
                                            contents.size() - written,
                                            static_cast<off_t>(written));
            if (result < 0) {
                if (errno == EINTR) {
                    continue;
                }
                throw std::runtime_error(std::string("pwrite: ") + std::strerror(errno));
            }
            written += static_cast<std::size_t>(result);
        }
    }

    TestFile(const TestFile&) = delete;
    TestFile& operator=(const TestFile&) = delete;

    ~TestFile() {
        if (fd_ >= 0) {
            ::close(fd_);
        }
    }

    [[nodiscard]] int fd() const noexcept { return fd_; }
    [[nodiscard]] std::vector<std::byte> expected(std::uint64_t offset,
                                                  std::size_t length) const {
        std::vector<std::byte> result;
        const std::size_t available =
            offset >= size_ ? 0 : std::min(length, size_ - static_cast<std::size_t>(offset));
        result.resize(available);
        for (std::size_t index = 0; index < available; ++index) {
            result[index] = static_cast<std::byte>((offset + index) % 251U);
        }
        return result;
    }

private:
    int fd_{-1};
    std::size_t size_{};
};

void expect(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void expect_result(const intentio::ReadResult& result,
                   const std::vector<std::byte>& expected,
                   const std::string& label) {
    expect(result.ok(), label + " returned an error: " + result.error.message());
    if (result.data != expected) {
        const unsigned actual = result.data.empty()
                                    ? 0U
                                    : std::to_integer<unsigned>(result.data.front());
        const unsigned wanted = expected.empty()
                                    ? 0U
                                    : std::to_integer<unsigned>(expected.front());
        throw std::runtime_error(label + " returned unexpected bytes (actual=" +
                                 std::to_string(actual) + ", expected=" +
                                 std::to_string(wanted) + ", actual_size=" +
                                 std::to_string(result.data.size()) + ", expected_size=" +
                                 std::to_string(expected.size()) + ")");
    }
}

intentio::ReadResult naive_read(const intentio::ReadRequest& request) {
    intentio::ReadResult result{
        request.id,
        request.offset,
        request.length,
        std::vector<std::byte>(request.length),
        {},
    };
    if (request.length == 0) {
        return result;
    }

    std::size_t completed = 0;
    while (completed < request.length) {
        const std::size_t remaining = request.length - completed;
        const ssize_t read_count = ::pread(request.fd,
                                           result.data.data() + completed,
                                           remaining,
                                           static_cast<off_t>(request.offset + completed));
        if (read_count < 0) {
            if (errno == EINTR) {
                continue;
            }
            result.error = std::error_code(errno, std::generic_category());
            result.data.clear();
            return result;
        }
        if (read_count == 0) {
            break;
        }
        completed += static_cast<std::size_t>(read_count);
        if (static_cast<std::size_t>(read_count) < remaining) {
            break;
        }
    }
    result.data.resize(completed);
    return result;
}

void expect_same_result(const intentio::ReadResult& actual,
                        const intentio::ReadResult& expected,
                        const std::string& label) {
    expect(actual.request_id == expected.request_id, label + " request id mismatch");
    expect(actual.offset == expected.offset, label + " offset mismatch");
    expect(actual.requested_bytes == expected.requested_bytes,
           label + " requested size mismatch");
    expect(actual.error.value() == expected.error.value(), label + " error mismatch");
    expect(actual.data == expected.data, label + " data mismatch");
}

void test_basic_reads_and_errors() {
    TestFile file(1024);
    intentio::Runtime runtime;
    auto group = runtime.create_group();
    group.read(file.fd(), 10, 32);
    group.read(file.fd(), 1000, 64);
    group.read(file.fd(), 0, 0);
    group.read(-1, 0, 8);

    const auto results = group.submit();
    expect(results.size() == 4, "basic result count");
    expect_result(results[0], file.expected(10, 32), "normal read");
    expect_result(results[1], file.expected(1000, 64), "EOF read");
    expect(results[1].bytes_read() == 24, "EOF read should be short");
    expect_result(results[2], {}, "zero-length read");
    expect(!results[3].ok(), "invalid descriptor should report an error");
    expect(results[3].error.value() == EBADF, "invalid descriptor error should be EBADF");

    intentio::Runtime eof_runtime;
    auto eof_group = eof_runtime.create_group();
    eof_group.read(file.fd(), 1000, 16);
    eof_group.read(file.fd(), 1016, 16);
    const auto eof_results = eof_group.submit();
    expect_result(eof_results[0], file.expected(1000, 16), "coalesced EOF first");
    expect_result(eof_results[1], file.expected(1016, 16), "coalesced EOF second");
    expect(eof_runtime.metrics().physical_reads == 1,
           "adjacent EOF reads should share one physical read");
}

void test_adjacent_coalescing() {
    TestFile file(4096);

    intentio::RuntimeOptions disabled_options;
    disabled_options.coalescing.enabled = false;
    intentio::Runtime disabled(disabled_options);
    auto disabled_group = disabled.create_group();
    disabled_group.read(file.fd(), 128, 64);
    disabled_group.read(file.fd(), 0, 64);
    disabled_group.read(file.fd(), 64, 64);
    const auto disabled_results = disabled_group.submit();
    expect_result(disabled_results[0], file.expected(128, 64), "disabled first");
    expect_result(disabled_results[1], file.expected(0, 64), "disabled second");
    expect_result(disabled_results[2], file.expected(64, 64), "disabled third");
    expect(disabled.metrics().physical_reads == 3, "disabled coalescing physical count");

    intentio::Runtime enabled;
    auto enabled_group = enabled.create_group();
    enabled_group.read(file.fd(), 128, 64);
    enabled_group.read(file.fd(), 0, 64);
    enabled_group.read(file.fd(), 64, 64);
    const auto enabled_results = enabled_group.submit();
    expect_result(enabled_results[0], file.expected(128, 64), "enabled first");
    expect_result(enabled_results[1], file.expected(0, 64), "enabled second");
    expect_result(enabled_results[2], file.expected(64, 64), "enabled third");
    expect(enabled.metrics().physical_reads == 1, "enabled coalescing physical count");
    expect(enabled.metrics().requests_merged == 2, "enabled merge count");
    expect(enabled.metrics().physical_bytes == 192, "enabled physical bytes");
}

void test_gap_fd_and_size_limits() {
    TestFile file(4096);
    const int second_fd = ::dup(file.fd());
    expect(second_fd >= 0, "dup should succeed");

    intentio::RuntimeOptions options;
    options.coalescing.max_gap_bytes = 4;
    options.coalescing.max_request_bytes = 12;
    intentio::Runtime runtime(options);
    auto group = runtime.create_group();
    group.read(file.fd(), 0, 4);
    group.read(file.fd(), 8, 4);
    group.read(second_fd, 12, 4);
    group.read(file.fd(), 16, 8);
    const auto results = group.submit();
    ::close(second_fd);

    expect_result(results[0], file.expected(0, 4), "gap first");
    expect_result(results[1], file.expected(8, 4), "gap second");
    expect_result(results[2], file.expected(12, 4), "different fd");
    expect_result(results[3], file.expected(16, 8), "size-limited read");
    expect(runtime.metrics().physical_reads == 3,
           "gap, fd, and maximum-size rules should produce three physical reads");
    expect(runtime.metrics().physical_bytes == 24,
           "maximum request size should prevent the second gap from being merged");
}

void test_access_pattern() {
    TestFile file(4096);
    intentio::RuntimeOptions options;
    options.coalescing.max_gap_bytes = 4;

    intentio::Runtime sequential(options);
    auto sequential_group = sequential.create_group();
    sequential_group.hint(intentio::AccessPattern::Sequential);
    sequential_group.read(file.fd(), 0, 4);
    sequential_group.read(file.fd(), 8, 4);
    const auto sequential_results = sequential_group.submit();
    expect_result(sequential_results[0], file.expected(0, 4), "sequential first");
    expect_result(sequential_results[1], file.expected(8, 4), "sequential second");
    expect(sequential.metrics().physical_reads == 1,
           "sequential access should retain the configured merge gap");

    intentio::Runtime random(options);
    auto random_group = random.create_group();
    random_group.hint(intentio::AccessPattern::Random);
    random_group.read(file.fd(), 0, 4);
    random_group.read(file.fd(), 8, 4);
    const auto random_results = random_group.submit();
    expect_result(random_results[0], file.expected(0, 4), "random first");
    expect_result(random_results[1], file.expected(8, 4), "random second");
    expect(random.metrics().physical_reads == 2,
           "random access should not merge configured gaps");
}

void test_intent_policy() {
    TestFile file(65536);
    constexpr std::uint64_t first_offset = 0;
    constexpr std::uint64_t second_offset = 4 * 1024 + 6 * 1024;

    intentio::Runtime unknown;
    auto unknown_group = unknown.create_group();
    unknown_group.read(file.fd(), first_offset, 4 * 1024);
    unknown_group.read(file.fd(), second_offset, 4 * 1024);
    const auto unknown_results = unknown_group.submit();
    expect_result(unknown_results[0], file.expected(first_offset, 4 * 1024),
                  "unknown policy first");
    expect_result(unknown_results[1], file.expected(second_offset, 4 * 1024),
                  "unknown policy second");
    expect(unknown.metrics().physical_reads == 2,
           "unknown policy should reject the six KiB gap");
    expect(unknown.metrics().merges_rejected_gap == 1,
           "unknown policy gap rejection metric");

    intentio::Runtime sequential;
    auto sequential_group = sequential.create_group();
    sequential_group.access_pattern(intentio::AccessPattern::Sequential);
    sequential_group.read(file.fd(), first_offset, 4 * 1024);
    sequential_group.read(file.fd(), second_offset, 4 * 1024);
    const auto sequential_results = sequential_group.submit();
    expect_result(sequential_results[0], file.expected(first_offset, 4 * 1024),
                  "sequential policy first");
    expect_result(sequential_results[1], file.expected(second_offset, 4 * 1024),
                  "sequential policy second");
    expect(sequential.metrics().physical_reads == 1,
           "sequential policy should accept a profitable gap");
    expect(sequential.metrics().read_amplification <= 1.75,
           "sequential policy amplification budget");

    intentio::Runtime amplification_guard;
    auto amplification_group = amplification_guard.create_group();
    amplification_group.access_pattern(intentio::AccessPattern::Sequential);
    amplification_group.read(file.fd(), 0, 4 * 1024);
    amplification_group.read(file.fd(), 32 * 1024, 4 * 1024);
    const auto amplification_results = amplification_group.submit();
    expect_result(amplification_results[0], file.expected(0, 4 * 1024),
                  "amplification guard first");
    expect_result(amplification_results[1], file.expected(32 * 1024, 4 * 1024),
                  "amplification guard second");
    expect(amplification_guard.metrics().physical_reads == 2,
           "incorrect sequential hint must not force a high-amplification merge");
    expect(amplification_guard.metrics().merges_rejected == 1,
           "aggregate merge rejection metric");
    expect(amplification_guard.metrics().merges_rejected_amplification == 1,
           "amplification rejection metric");

    intentio::Runtime random;
    auto random_group = random.create_group();
    random_group.access_pattern(intentio::AccessPattern::Random);
    random_group.read(file.fd(), first_offset, 4 * 1024);
    random_group.read(file.fd(), second_offset, 4 * 1024);
    random_group.read(file.fd(), 512, 256);
    random_group.read(file.fd(), 512, 256);
    const auto random_results = random_group.submit();
    expect_result(random_results[0], file.expected(first_offset, 4 * 1024),
                  "random policy first");
    expect_result(random_results[1], file.expected(second_offset, 4 * 1024),
                  "random policy second");
    expect_result(random_results[2], file.expected(512, 256),
                  "random policy duplicate first");
    expect_result(random_results[3], file.expected(512, 256),
                  "random policy duplicate second");
    expect(random.metrics().physical_reads == 2,
           "random policy should preserve exact dedup while rejecting gaps");
    expect(random.metrics().requests_deduplicated == 2,
           "random policy exact dedup metric");

    intentio::Runtime latency_sensitive;
    auto latency_group = latency_sensitive.create_group();
    latency_group.latency_class(intentio::LatencyClass::LatencySensitive);
    latency_group.read(file.fd(), first_offset, 4 * 1024);
    latency_group.read(file.fd(), second_offset, 4 * 1024);
    const auto latency_results = latency_group.submit();
    expect_result(latency_results[0], file.expected(first_offset, 4 * 1024),
                  "latency policy first");
    expect_result(latency_results[1], file.expected(second_offset, 4 * 1024),
                  "latency policy second");
    expect(latency_sensitive.metrics().physical_reads == 2,
           "latency-sensitive policy should reject speculative gaps");

    intentio::Runtime throughput;
    auto throughput_group = throughput.create_group();
    throughput_group.latency_class(intentio::LatencyClass::Throughput);
    throughput_group.read(file.fd(), first_offset, 4 * 1024);
    throughput_group.read(file.fd(), second_offset, 4 * 1024);
    const auto throughput_results = throughput_group.submit();
    expect_result(throughput_results[0], file.expected(first_offset, 4 * 1024),
                  "throughput policy first");
    expect_result(throughput_results[1], file.expected(second_offset, 4 * 1024),
                  "throughput policy second");
    expect(throughput.metrics().physical_reads == 1,
           "throughput policy should accept a bounded gap");

    intentio::Runtime scan_once;
    auto scan_once_group = scan_once.create_group();
    scan_once_group.reuse_hint(intentio::ReuseHint::ScanOnce);
    scan_once_group.read(file.fd(), first_offset, 4 * 1024);
    scan_once_group.read(file.fd(), second_offset, 4 * 1024);
    const auto scan_once_results = scan_once_group.submit();
    expect_result(scan_once_results[0], file.expected(first_offset, 4 * 1024),
                  "scan-once policy first");
    expect_result(scan_once_results[1], file.expected(second_offset, 4 * 1024),
                  "scan-once policy second");
    expect(scan_once.metrics().physical_reads == 1,
           "scan-once policy should use a throughput-oriented merge budget");
}

void test_exact_deduplication() {
    TestFile file(4096);
    intentio::Runtime runtime;
    auto group = runtime.create_group();
    group.read(file.fd(), 512, 64);
    group.read(file.fd(), 512, 64);
    group.read(file.fd(), 512, 64);
    const auto results = group.submit();
    for (const auto& result : results) {
        expect_result(result, file.expected(512, 64), "exact duplicate");
    }

    const auto metrics = runtime.metrics();
    expect(metrics.physical_reads == 1, "exact duplicates should use one physical read");
    expect(metrics.requests_deduplicated == 2, "exact duplicate request count");
    expect(metrics.physical_reads_avoided_by_dedup == 2,
           "exact duplicate physical-read savings");
    expect(metrics.bytes_avoided_by_dedup == 128, "exact duplicate byte savings");
    expect(metrics.unique_logical_bytes == 64, "exact duplicate unique bytes");
    expect(std::abs(metrics.read_amplification - 1.0) < 0.000001,
           "exact duplicate read amplification");

    intentio::RuntimeOptions disabled_options;
    disabled_options.deduplication.enabled = false;
    intentio::Runtime disabled(disabled_options);
    auto disabled_group = disabled.create_group();
    disabled_group.read(file.fd(), 512, 64);
    disabled_group.read(file.fd(), 512, 64);
    disabled_group.read(file.fd(), 512, 64);
    const auto disabled_results = disabled_group.submit();
    for (const auto& result : disabled_results) {
        expect_result(result, file.expected(512, 64), "dedup disabled");
    }
    expect(disabled.metrics().physical_reads == 3,
           "disabling dedup should restore independent reads");
    expect(disabled.metrics().requests_deduplicated == 0,
           "dedup disabled metric");
}

void test_contained_and_partial_overlap() {
    TestFile file(4096);

    intentio::Runtime contained;
    auto contained_group = contained.create_group();
    contained_group.read(file.fd(), 256, 512);
    contained_group.read(file.fd(), 0, 1024);
    contained_group.read(file.fd(), 512, 256);
    const auto contained_results = contained_group.submit();
    expect_result(contained_results[0], file.expected(256, 512), "contained first");
    expect_result(contained_results[1], file.expected(0, 1024), "contained parent");
    expect_result(contained_results[2], file.expected(512, 256), "contained second");
    expect(contained.metrics().physical_reads == 1,
           "contained ranges should use the parent physical read");
    expect(contained.metrics().requests_deduplicated == 2,
           "contained ranges dedup count");
    expect(contained.metrics().bytes_avoided_by_dedup == 768,
           "contained ranges byte savings");

    intentio::Runtime partial;
    auto partial_group = partial.create_group();
    partial_group.read(file.fd(), 64, 64);
    partial_group.read(file.fd(), 0, 64);
    partial_group.read(file.fd(), 32, 64);
    const auto partial_results = partial_group.submit();
    expect_result(partial_results[0], file.expected(64, 64), "partial third");
    expect_result(partial_results[1], file.expected(0, 64), "partial first");
    expect_result(partial_results[2], file.expected(32, 64), "partial second");
    expect(partial.metrics().physical_reads == 1,
           "partial overlaps should use one union physical read");
    expect(partial.metrics().physical_bytes == 128,
           "partial overlap physical union size");
    expect(partial.metrics().unique_logical_bytes == 128,
           "partial overlap unique logical size");
    expect(partial.metrics().bytes_avoided_by_dedup == 64,
           "partial overlap byte savings");
}

void test_overlap_limits_and_file_descriptors() {
    TestFile file(4096);
    const int other_fd = ::dup(file.fd());
    expect(other_fd >= 0, "dup should succeed for descriptor isolation test");

    intentio::RuntimeOptions limited_options;
    limited_options.coalescing.max_request_bytes = 80;
    intentio::Runtime limited(limited_options);
    auto limited_group = limited.create_group();
    limited_group.read(file.fd(), 0, 64);
    limited_group.read(file.fd(), 32, 64);
    const auto limited_results = limited_group.submit();
    expect_result(limited_results[0], file.expected(0, 64), "limited first");
    expect_result(limited_results[1], file.expected(32, 64), "limited second");
    expect(limited.metrics().physical_reads == 2,
           "maximum merged size must prevent oversized overlap unions");
    expect(limited.metrics().requests_deduplicated == 0,
           "rejected overlap must not count as deduplicated");

    intentio::Runtime descriptors;
    auto descriptor_group = descriptors.create_group();
    descriptor_group.read(file.fd(), 128, 64);
    descriptor_group.read(other_fd, 128, 64);
    const auto descriptor_results = descriptor_group.submit();
    expect_result(descriptor_results[0], file.expected(128, 64), "first fd");
    expect_result(descriptor_results[1], file.expected(128, 64), "second fd");
    expect(descriptors.metrics().physical_reads == 2,
           "different descriptors must not deduplicate");
    ::close(other_fd);
}

void test_randomized_differential() {
    TestFile file(16384);
    const int other_fd = ::dup(file.fd());
    expect(other_fd >= 0, "dup should succeed for randomized test");

    struct DifferentialSeed {
        std::uint32_t value;
        std::size_t groups;
    };
    constexpr std::array<DifferentialSeed, 3> seeds{{
        {482193, 3000},
        {917331, 500},
        {381917, 500},
    }};
    constexpr std::array<const char*, 5> intent_names{{
        "Unknown",
        "Sequential",
        "Random",
        "LatencySensitive",
        "Throughput",
    }};

    for (const DifferentialSeed seed : seeds) {
        std::mt19937 generator(seed.value);
        std::uniform_int_distribution<std::size_t> request_count_distribution(1, 24);
        std::uniform_int_distribution<std::uint64_t> offset_distribution(0, 18000);
        std::uniform_int_distribution<std::size_t> length_distribution(0, 1024);
        std::uniform_int_distribution<int> fd_distribution(0, 9);
        std::uniform_int_distribution<std::uint64_t> gap_distribution(0, 32);
        std::uniform_int_distribution<std::size_t> max_size_distribution(256, 4096);

        for (std::size_t iteration = 0; iteration < seed.groups; ++iteration) {
            const std::uint64_t max_gap = gap_distribution(generator);
            const std::size_t max_request_size = max_size_distribution(generator);
            struct RequestSpec {
                int fd;
                std::uint64_t offset;
                std::size_t length;
            };
            std::vector<RequestSpec> specs;
            const std::size_t request_count = request_count_distribution(generator);
            specs.reserve(request_count);
            for (std::size_t index = 0; index < request_count; ++index) {
                const int fd_choice = fd_distribution(generator);
                specs.push_back(RequestSpec{
                    fd_choice == 0 ? -1 : (fd_choice < 5 ? file.fd() : other_fd),
                    offset_distribution(generator),
                    length_distribution(generator),
                });
            }

            for (std::size_t intent_index = 0; intent_index < intent_names.size();
                 ++intent_index) {
                intentio::RuntimeOptions options;
                options.coalescing.max_gap_bytes = max_gap;
                options.coalescing.max_request_bytes = max_request_size;
                intentio::Runtime runtime(options);
                auto group = runtime.create_group();
                switch (intent_index) {
                case 0:
                    break;
                case 1:
                    group.access_pattern(intentio::AccessPattern::Sequential);
                    break;
                case 2:
                    group.access_pattern(intentio::AccessPattern::Random);
                    break;
                case 3:
                    group.latency_class(intentio::LatencyClass::LatencySensitive);
                    break;
                case 4:
                    group.latency_class(intentio::LatencyClass::Throughput);
                    break;
                }

                std::vector<intentio::ReadRequest> requests;
                requests.reserve(specs.size());
                for (const RequestSpec& spec : specs) {
                    const auto id = group.read(spec.fd, spec.offset, spec.length);
                    requests.push_back(
                        intentio::ReadRequest{id, spec.fd, spec.offset, spec.length});
                }

                const auto actual = group.submit();
                expect(actual.size() == requests.size(), "randomized result count");
                for (std::size_t index = 0; index < requests.size(); ++index) {
                    const auto expected = naive_read(requests[index]);
                    expect_same_result(
                        actual[index], expected,
                        "randomized seed=" + std::to_string(seed.value) +
                            " intent=" + intent_names[intent_index] +
                            " iteration=" + std::to_string(iteration) +
                            " request=" + std::to_string(index));
                }
            }
        }
    }
    ::close(other_fd);
}

void test_auto_backend() {
    TestFile file(4096);
    intentio::RuntimeOptions options;
    options.backend = intentio::Backend::Auto;
    options.queue_depth = 8;
    intentio::Runtime runtime(options);
    auto group = runtime.create_group();
    group.read(file.fd(), 256, 128);
    const auto results = group.submit();
    expect_result(results[0], file.expected(256, 128), "auto backend");
}

} // namespace

int main() {
    try {
        test_basic_reads_and_errors();
        test_adjacent_coalescing();
        test_gap_fd_and_size_limits();
        test_access_pattern();
        test_intent_policy();
        test_exact_deduplication();
        test_contained_and_partial_overlap();
        test_overlap_limits_and_file_descriptors();
        test_randomized_differential();
        test_auto_backend();
        std::cout << "IntentIO tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "IntentIO tests failed: " << error.what() << '\n';
        return 1;
    }
}
