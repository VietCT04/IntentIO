#include <intentio/intentio.hpp>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <iostream>
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
        test_auto_backend();
        std::cout << "IntentIO tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "IntentIO tests failed: " << error.what() << '\n';
        return 1;
    }
}
